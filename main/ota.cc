#include "ota.h"
#include "cloudzao_endpoints.h"
#include "display.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "http_client.h"

#include <memory>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_task_wdt.h>
#include "ota_screen.h"
#include "esp_lv_adapter.h"
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <cstring>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <sstream>
#include <algorithm>
#include <sys/time.h>

#define TAG "Ota"

namespace {

class LvglPauseGuard {
public:
    LvglPauseGuard() {
        if (OtaScreen::IsActive()) {
            return;
        }
        if (esp_lv_adapter_is_initialized()) {
            if (esp_lv_adapter_pause(-1) == ESP_OK) {
                paused_ = true;
            }
        }
    }

    ~LvglPauseGuard() {
        if (paused_) {
            esp_lv_adapter_resume();
        }
    }

private:
    bool paused_ = false;
};

const char* ModemHttpErrorToString(int error_code) {
    switch (error_code) {
        case 1: return "DNS resolution failed";
        case 2: return "Connection failed";
        case 3: return "Connect timeout";
        case 4: return "SSL handshake failed";
        case 5: return "Connection aborted";
        case 6: return "Response timeout";
        case 7: return "Parse failed";
        case 8: return "Cache insufficient";
        case 9: return "Packet loss";
        case 10: return "File write failed";
        case 255: return "Unknown error";
        case -1: return "no modem error";
        default: return "Undefined error";
    }
}

bool IsModemTransportError(esp_err_t err) {
    switch (static_cast<int>(err)) {
        case 1:   // DNS
        case 2:   // connect failed
        case 4:   // SSL handshake
        case 255: // unknown / catch-all
            return true;
        default:
            return false;
    }
}

esp_err_t NormalizeTransportError(int last_error) {
    if (last_error > 0) {
        return last_error;
    }
    return 255;
}

}  // namespace


Ota::Ota() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // Read Serial Number from efuse user_data
    uint8_t serial_number[33] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        if (serial_number[0] == 0) {
            has_serial_number_ = false;
        } else {
            serial_number_ = std::string(reinterpret_cast<char*>(serial_number), 32);
            has_serial_number_ = true;
        }
    }
#endif
}

Ota::~Ota() {
}

std::string Ota::GetCheckVersionUrl() {
    Settings settings("wifi", true);
    std::string url = settings.GetString("ota_url");
    // NVS 未配置或包含旧版业务地址时，使用官方 OTA 地址
    if (url.empty() || cloudzao_is_legacy_ota_url(url.c_str())) {
        url = xiaozhi_ota_url;
        settings.SetString("ota_url", url);
    }
    return url;
}

std::unique_ptr<Http> Ota::SetupHttp(bool use_socket_http) {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    std::unique_ptr<Http> http;
    if (use_socket_http) {
        http = std::make_unique<HttpClient>(network, 0);
    } else {
        http = network->CreateHttp(0);
    }
    auto user_agent = SystemInfo::GetUserAgent();
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "Setup HTTP, http_impl=%s User-Agent: %s, Serial-Number: %s",
                 use_socket_http ? "socket" : "mhttp", user_agent.c_str(), serial_number_.c_str());
    } else {
        ESP_LOGI(TAG, "Setup HTTP, http_impl=%s User-Agent: %s",
                 use_socket_http ? "socket" : "mhttp", user_agent.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");

    return http;
}

esp_err_t Ota::ExecuteHttp(bool use_socket_http, const std::string& method, const std::string& url,
                           const std::string& content, std::string* response_body,
                           bool read_error_body) {
    const char* impl = use_socket_http ? "socket" : "mhttp";
    auto http = SetupHttp(use_socket_http);
    if (!content.empty()) {
        http->SetContent(std::string(content));
    }

    if (!http->Open(method, url)) {
        int last_error = http->GetLastError();
        ESP_LOGE(TAG, "Network operation failed");
        http->Close();
        return NormalizeTransportError(last_error);
    }

    auto status_code = http->GetStatusCode();
    if (status_code == -1) {
        const int last_error = http->GetLastError();
        http->Close();
        ESP_LOGE(TAG, "Network operation failed");
        return NormalizeTransportError(last_error);
    }

    if (status_code != 200) {
        std::string err_body;
        if (read_error_body) {
            err_body = http->ReadAll();
        }
        http->Close();
        ESP_LOGE(TAG, "Network operation failed");
        if (response_body) {
            *response_body = std::move(err_body);
        }
        return status_code;
    }

    if (response_body) {
        *response_body = http->ReadAll();
    }
    http->Close();
    ESP_LOGI(TAG, "HTTP OK status=200 body_len=%u http_impl=%s",
             static_cast<unsigned>(response_body ? response_body->size() : 0), impl);
    return ESP_OK;
}

/* 
 * Specification: https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
esp_err_t Ota::CheckVersion() {
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();

    // Check if there is a new firmware version available
    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());

    std::string url = GetCheckVersionUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return ESP_ERR_INVALID_ARG;
    }

    std::string request_body = board.GetSystemInfoJson();
    if (!request_body.empty()) {
        cJSON* probe = cJSON_Parse(request_body.c_str());
        if (probe == nullptr) {
            ESP_LOGE(TAG,
                     "GetSystemInfoJson produced invalid JSON (len=%u), preview=%.256s",
                     static_cast<unsigned>(request_body.size()), request_body.c_str());
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_Delete(probe);
    }
    std::string method = request_body.length() > 0 ? "POST" : "GET";

    std::string data;
    esp_err_t http_err = ExecuteHttp(false, method, url, request_body, &data, true);
    if (http_err != ESP_OK && IsModemTransportError(http_err)) {
        ESP_LOGW(TAG, "CheckVersion MHTTP failed err=%d (%s), retry via socket HTTP",
                 static_cast<int>(http_err), ModemHttpErrorToString(http_err));
        data.clear();
        http_err = ExecuteHttp(true, method, url, request_body, &data, true);
    }
    if (http_err != ESP_OK) {
        return http_err;
    }

    // Response: { "firmware": { "version": "1.0.0", "url": "http://" } }
    // Parse the JSON response and check if the version is newer
    // If it is, set has_new_version_ to true and store the new version and URL
    
    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    has_activation_code_ = false;
    has_activation_challenge_ = false;
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms)) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
    }

    has_mqtt_config_ = false;
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, mqtt) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_mqtt_config_ = true;
    } else {
        ESP_LOGI(TAG, "No mqtt section found !");
    }

    has_websocket_config_ = false;
    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, websocket) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_websocket_config_ = true;
    } else {
        ESP_LOGI(TAG, "No websocket section found!");
    }

    has_server_time_ = false;
    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        
        if (cJSON_IsNumber(timestamp)) {
            // timestamp 为 UTC 毫秒；settimeofday 要 UTC。勿再加 timezone_offset，
            // 否则与 TZ=CST-8 的 localtime 叠成 +16h（如 16:19 显示成 00:19）。
            struct timeval tv;
            double ts = timestamp->valuedouble;
            tv.tv_sec = (time_t)(ts / 1000);
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;
            settimeofday(&tv, NULL);
            setenv("TZ", "CST-8", 1);
            tzset();
            has_server_time_ = true;
        }
    } else {
        ESP_LOGW(TAG, "No server_time section found!");
    }

    has_new_version_ = false;
    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON *version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        cJSON *url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url)) {
            firmware_url_ = url->valuestring;
        }

        if (cJSON_IsString(version) && cJSON_IsString(url)) {
            // Check if the version is newer, for example, 0.1.0 is newer than 0.0.1
            has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
            if (has_new_version_) {
                ESP_LOGI(TAG, "New version available: %s", firmware_version_.c_str());
            } else {
                ESP_LOGI(TAG, "Current is the latest version");
            }
            // If the force flag is set to 1, the given version is forced to be installed
            cJSON *force = cJSON_GetObjectItem(firmware, "force");
            if (cJSON_IsNumber(force) && force->valueint == 1) {
                has_new_version_ = true;
            }
        }
    } else {
        ESP_LOGW(TAG, "No firmware section found!");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool Ota::Upgrade(const std::string& firmware_url) {
    LvglPauseGuard lvgl_pause;
    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label, update_partition->address);
    constexpr size_t kAppDescOffset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    constexpr size_t kMinHeaderSize = kAppDescOffset + sizeof(esp_app_desc_t);
    bool image_header_checked = false;
    char pending_buf[1024];
    size_t pending_len = 0;

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", http->GetStatusCode());
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        return false;
    }

    char buffer[512];
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    while (true) {
        // esp_task_wdt_reset();
        int ret = http->Read(buffer, sizeof(buffer));
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            return false;
        }

        recent_read += ret;
        total_read += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, (unsigned)total_read, (unsigned)content_length, (unsigned)recent_read);
            if (upgrade_callback_) {
                upgrade_callback_(progress, total_read, content_length, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (ret == 0) {
            break;
        }

        if (!image_header_checked) {
            if (pending_len + ret > sizeof(pending_buf)) {
                ESP_LOGE(TAG, "Pending buffer overflow: %u + %d", (unsigned)pending_len, ret);
                return false;
            }
            memcpy(pending_buf + pending_len, buffer, ret);
            pending_len += ret;
            if (pending_len >= kMinHeaderSize) {
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, pending_buf + kAppDescOffset, sizeof(esp_app_desc_t));

                auto current_version = esp_app_get_description()->version;
                ESP_LOGI(TAG, "Current version: %s, New version: %s", current_version, new_app_info.version);

                esp_err_t begin_err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
                if (begin_err != ESP_OK) {
                    esp_ota_abort(update_handle);
                    ESP_LOGE(TAG, "Failed to begin OTA: %s", esp_err_to_name(begin_err));
                    return false;
                }

                image_header_checked = true;
                auto err = esp_ota_write(update_handle, pending_buf, pending_len);
                pending_len = 0;
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Network operation failed");
                    esp_ota_abort(update_handle);
                    return false;
                }
                continue;
            }
            continue;
        }

        auto err = esp_ota_write(update_handle, buffer, ret);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Network operation failed");
            esp_ota_abort(update_handle);
            return false;
        }
    }
    http->Close();

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Network operation failed");
        }
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Network operation failed");
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
}

bool Ota::StartUpgrade(OtaProgressCallback callback) {
    upgrade_callback_ = std::move(callback);
    return Upgrade(firmware_url_);
}

bool Ota::StartUpgradeFromUrl(const std::string& url, OtaProgressCallback callback) {
    upgrade_callback_ = std::move(callback);
    return Upgrade(url);
}

std::vector<int> Ota::ParseVersion(const std::string& version) {
    std::vector<int> versionNumbers;
    std::stringstream ss(version);
    std::string segment;
    
    while (std::getline(ss, segment, '.')) {
        versionNumbers.push_back(std::stoi(segment));
    }
    
    return versionNumbers;
}

bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    std::vector<int> current = ParseVersion(currentVersion);
    std::vector<int> newer = ParseVersion(newVersion);
    
    for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i) {
        if (newer[i] > current[i]) {
            return true;
        } else if (newer[i] < current[i]) {
            return false;
        }
    }
    
    return newer.size() > current.size();
}

std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32]; // SHA-256 输出为32字节
    
    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(), activation_challenge_.size(), hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);
    return json;
}

esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    if (url.back() != '/') {
        url += xiaozhi_path_activate;
    } else {
        url += xiaozhi_path_activate + 1;
    }

    std::string data = GetActivationPayload();
    auto try_activate = [&](bool use_socket_http) -> esp_err_t {
        const char* impl = use_socket_http ? "socket" : "mhttp";
        auto http = SetupHttp(use_socket_http);
        http->SetContent(std::string(data));
        if (!http->Open("POST", url)) {
            int last_error = http->GetLastError();
            ESP_LOGE(TAG, "Failed to open HTTP connection http_impl=%s modem_err=%d (%s)",
                     impl, last_error, ModemHttpErrorToString(last_error));
            http->Close();
            return NormalizeTransportError(last_error);
        }

        auto status_code = http->GetStatusCode();
        if (status_code == -1) {
            int last_error = http->GetLastError();
            http->Close();
            ESP_LOGE(TAG, "Failed to activate, status=-1 http_impl=%s modem_err=%d (%s)",
                     impl, last_error, ModemHttpErrorToString(last_error));
            return NormalizeTransportError(last_error);
        }
        if (status_code == 202) {
            http->Close();
            return ESP_ERR_TIMEOUT;
        }
        if (status_code != 200) {
            ESP_LOGE(TAG, "Failed to activate, code: %d, http_impl=%s body: %s",
                     status_code, impl, http->ReadAll().c_str());
            http->Close();
            return status_code;
        }
        http->Close();
        return ESP_OK;
    };

    esp_err_t err = try_activate(false);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT && IsModemTransportError(err)) {
        ESP_LOGW(TAG, "Network operation failed");
        err = try_activate(true);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Activation successful");
    }
    return err;
}
