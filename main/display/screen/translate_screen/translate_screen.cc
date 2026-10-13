#include "translate_screen.h"
#include "i18n.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "api_endpoints.h"
#include "application.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "board.h"
#include "home_screen/home_screen.h"
#include "http.h"
#include "panel_metrics.h"
#include "screen_util.h"
#include "system_info.h"
#include <web_socket.h>

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "TranslateScreen";

constexpr int kSampleRate = 16000;
constexpr int kFrameMs = 100;
constexpr int kSamplesPerFrame = kSampleRate * kFrameMs / 1000;  // 1600
constexpr int kMinPcmBytes = 80;
constexpr int kHttpTimeoutMs = 20000;
constexpr int kEndWaitMs = 5000;
// 建连期间预缓冲上限（PSRAM），约 1.5s @16kHz mono（4G 冲刷不宜更长）
constexpr size_t kPrebufferSamples =
    static_cast<size_t>(kSampleRate) + static_cast<size_t>(kSampleRate) / 2;

constexpr int32_t kPanelW = 240;
constexpr int32_t kPanelH = 240;
constexpr int32_t kHeaderH = 36;
constexpr int32_t kFooterH = 34;
constexpr int32_t kLangRowH = 28;
constexpr int32_t kStatusH = 18;
constexpr int32_t kBodyH = kPanelH - kHeaderH - kFooterH;
constexpr int32_t kHeaderSidePad = 6;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorHeaderBg = 0x12151C;
constexpr uint32_t kColorDivider = 0x2A2F3A;
constexpr uint32_t kColorCard = 0x1A1F2A;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorHintText = 0x9AA3B2;
constexpr uint32_t kColorTrans = 0x93C5FD;
constexpr uint32_t kColorBtnIdle = 0x2563EB;
constexpr uint32_t kColorBtnActive = 0xDC2626;
constexpr uint32_t kColorBtnBusy = 0x4B5563;

// Sonicloud 语种表 lanid 0–99（fromlan/tolan 不支持 >=100）
// https://open.sinicloud.com/documents/webapi/stream-asr/#语种编号列表lanid
struct LangEntry {
    int lanid;
    const char* code;
    const char* name_zh;
};

constexpr LangEntry kLangs[] = {
#include "sinicloud_lang_table.inc"
};
constexpr size_t kLangCount = sizeof(kLangs) / sizeof(kLangs[0]);
constexpr int kDefaultFromIdx = 0;  // zh
constexpr int kDefaultToIdx = 1;    // en

enum class State : uint8_t {
    Idle,
    Connecting,
    Streaming,
    Stopping,
    Closing,
};

std::atomic<State> s_state{State::Idle};
// 硬取消：退屏 / 致命错误；会打断 WS Connect
std::atomic<bool> s_stop_requested{false};
// 松手结束：Connecting 阶段不打断建连，就绪后冲刷缓冲并 end
std::atomic<bool> s_release_requested{false};
std::atomic<bool> s_can_send_audio{false};
std::atomic<bool> s_capturing{false};
std::atomic<bool> s_got_final_result{false};
std::atomic<bool> s_got_session_error{false};
std::atomic<uint32_t> s_session{0};
std::mutex s_http_mutex;
std::mutex s_text_mutex;
std::mutex s_ws_mutex;
std::mutex s_prebuffer_mutex;

lv_obj_t* s_screen = nullptr;
lv_obj_t* s_from_dd = nullptr;
lv_obj_t* s_to_dd = nullptr;
lv_obj_t* s_from_name_lbl = nullptr;
lv_obj_t* s_to_name_lbl = nullptr;
lv_obj_t* s_status_lbl = nullptr;
lv_obj_t* s_source_lbl = nullptr;
lv_obj_t* s_trans_lbl = nullptr;
lv_obj_t* s_action_btn = nullptr;
lv_obj_t* s_action_lbl = nullptr;
TaskHandle_t s_worker_task = nullptr;
TaskHandle_t s_capture_task = nullptr;

// PSRAM 环形预缓冲（建连期间录音）
int16_t* s_prebuffer = nullptr;
size_t s_prebuffer_capacity = 0;
size_t s_prebuffer_head = 0;
size_t s_prebuffer_count = 0;

std::shared_ptr<WebSocket> s_ws;
std::string s_source_text;
std::string s_trans_text;
std::string s_lang_options;

const char* kFatalCodes[] = {
    "1",    "2",    "-1",   "-2",   "-3",   "-4",   "16",   "4000",
    "4001", "4002", "4003", "4004", "4005", "4006", "4007", "4010",
    "4011", "4012", "4013", "4014", "4015", "4016", "5000", "5001",
    "5002",
};

bool screen_alive() { return s_screen != nullptr; }

bool session_alive(uint32_t session) {
    return session == s_session.load(std::memory_order_acquire) &&
           s_state.load(std::memory_order_acquire) != State::Closing;
}

bool is_fatal_code(const char* code) {
    if (code == nullptr) {
        return false;
    }
    for (const char* c : kFatalCodes) {
        if (std::strcmp(c, code) == 0) {
            return true;
        }
    }
    return false;
}

struct WakeWordGuard {
    AudioService& as;
    bool disabled = false;
    explicit WakeWordGuard(AudioService& audio) : as(audio) {
        if (as.IsWakeWordRunning()) {
            as.EnableWakeWordDetection(false);
            disabled = true;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    ~WakeWordGuard() {
        // 仅当语音 UI 会话仍在时恢复，避免翻译页把桌面唤醒词误打开。
        if (disabled && Application::GetInstance().IsVoiceUiActive()) {
            as.EnableWakeWordDetection(true);
        }
    }
    WakeWordGuard(const WakeWordGuard&) = delete;
    WakeWordGuard& operator=(const WakeWordGuard&) = delete;
};

const std::string& device_user_id() {
    static const std::string kId = [] {
        std::string mac = SystemInfo::GetMacAddress();
        for (char& c : mac) {
            if (c == ':') {
                c = '-';
            }
        }
        return mac;
    }();
    return kId;
}

void keep_left_channel_only(std::vector<int16_t>& samples) {
    if (samples.size() < 2) {
        return;
    }
    const size_t mono_count = samples.size() / 2;
    for (size_t i = 0, j = 0; i < mono_count; ++i, j += 2) {
        samples[i] = samples[j];
    }
    samples.resize(mono_count);
}

bool prebuffer_ensure() {
    std::lock_guard<std::mutex> lock(s_prebuffer_mutex);
    if (s_prebuffer != nullptr && s_prebuffer_capacity == kPrebufferSamples) {
        return true;
    }
    if (s_prebuffer != nullptr) {
        heap_caps_free(s_prebuffer);
        s_prebuffer = nullptr;
    }
    s_prebuffer = static_cast<int16_t*>(heap_caps_malloc(
        kPrebufferSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_prebuffer == nullptr) {
        s_prebuffer_capacity = 0;
        s_prebuffer_head = 0;
        s_prebuffer_count = 0;
        ESP_LOGW(TAG, "prebuffer alloc failed");
        return false;
    }
    s_prebuffer_capacity = kPrebufferSamples;
    s_prebuffer_head = 0;
    s_prebuffer_count = 0;
    return true;
}

void prebuffer_clear() {
    std::lock_guard<std::mutex> lock(s_prebuffer_mutex);
    s_prebuffer_head = 0;
    s_prebuffer_count = 0;
}

size_t prebuffer_pending_count() {
    std::lock_guard<std::mutex> lock(s_prebuffer_mutex);
    return s_prebuffer_count;
}

void prebuffer_push(const int16_t* samples, size_t count) {
    if (samples == nullptr || count == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(s_prebuffer_mutex);
    if (s_prebuffer == nullptr || s_prebuffer_capacity == 0) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        s_prebuffer[s_prebuffer_head] = samples[i];
        s_prebuffer_head = (s_prebuffer_head + 1) % s_prebuffer_capacity;
        if (s_prebuffer_count < s_prebuffer_capacity) {
            ++s_prebuffer_count;
        }
    }
}

bool send_pcm_frame(const int16_t* samples, size_t count);

bool prebuffer_flush_send() {
    std::vector<int16_t> snapshot;
    {
        std::lock_guard<std::mutex> lock(s_prebuffer_mutex);
        if (s_prebuffer == nullptr || s_prebuffer_count == 0) {
            return true;
        }
        snapshot.resize(s_prebuffer_count);
        const size_t start =
            (s_prebuffer_head + s_prebuffer_capacity - s_prebuffer_count) %
            s_prebuffer_capacity;
        for (size_t i = 0; i < s_prebuffer_count; ++i) {
            snapshot[i] = s_prebuffer[(start + i) % s_prebuffer_capacity];
        }
        s_prebuffer_head = 0;
        s_prebuffer_count = 0;
    }
    ESP_LOGI(TAG, "flush prebuffer samples=%u", (unsigned)snapshot.size());
    const TickType_t flush_start = xTaskGetTickCount();
    for (size_t off = 0; off < snapshot.size();) {
        if (s_release_requested.load(std::memory_order_acquire) ||
            s_stop_requested.load(std::memory_order_acquire)) {
            ESP_LOGI(TAG,
                     "flush prebuffer abort off=%u total=%u (release/stop)",
                     (unsigned)off, (unsigned)snapshot.size());
            return true;
        }
        const size_t n =
            std::min(static_cast<size_t>(kSamplesPerFrame), snapshot.size() - off);
        const TickType_t send_start = xTaskGetTickCount();
        if (!send_pcm_frame(snapshot.data() + off, n)) {
            const uint32_t total_ms =
                (xTaskGetTickCount() - flush_start) * portTICK_PERIOD_MS;
            ESP_LOGW(TAG,
                     "flush prebuffer send failed off=%u total_ms=%u",
                     (unsigned)off, (unsigned)total_ms);
            return false;
        }
        const uint32_t send_ms =
            (xTaskGetTickCount() - send_start) * portTICK_PERIOD_MS;
        if (send_ms > 300) {
            ESP_LOGW(TAG, "flush frame slow off=%u send_ms=%u", (unsigned)off,
                     (unsigned)send_ms);
        }
        off += n;
        if (off < snapshot.size() && send_ms < static_cast<uint32_t>(kFrameMs)) {
            vTaskDelay(pdMS_TO_TICKS(kFrameMs - static_cast<int>(send_ms)));
        }
    }
    const uint32_t total_ms =
        (xTaskGetTickCount() - flush_start) * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "flush prebuffer done samples=%u total_ms=%u",
             (unsigned)snapshot.size(), (unsigned)total_ms);
    return true;
}

void stop_capture_task() {
    s_capturing.store(false, std::memory_order_release);
    for (int i = 0; i < 200 && s_capture_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_capture_task != nullptr) {
        ESP_LOGW(TAG, "capture task still running");
    }
}

void capture_task(void* arg) {
    const uint32_t session = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    auto& as = Application::GetInstance().GetAudioService();
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    std::vector<int16_t> frame;
    frame.reserve(static_cast<size_t>(kSamplesPerFrame) * 2);

    while (s_capturing.load(std::memory_order_acquire) &&
           !s_stop_requested.load(std::memory_order_acquire) &&
           session_alive(session)) {
        if (!as.ReadAudioData(frame, kSampleRate, kSamplesPerFrame)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (codec != nullptr && codec->input_channels() == 2) {
            keep_left_channel_only(frame);
        }
        prebuffer_push(frame.data(), frame.size());
    }

    s_capture_task = nullptr;
    vTaskDeleteWithCaps(nullptr);
}

bool start_capture_task(uint32_t session) {
    if (s_capture_task != nullptr) {
        return true;
    }
    if (!prebuffer_ensure()) {
        return false;
    }
    prebuffer_clear();
    s_capturing.store(true, std::memory_order_release);
    constexpr UBaseType_t kCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        capture_task, "translate_cap", 4096,
        reinterpret_cast<void*>(static_cast<uintptr_t>(session)), 5,
        &s_capture_task, 0, kCaps);
    if (ok != pdPASS) {
        s_capture_task = nullptr;
        s_capturing.store(false, std::memory_order_release);
        ESP_LOGW(TAG, "create capture task failed");
        return false;
    }
    return true;
}

void build_lang_options() {
    if (!s_lang_options.empty()) {
        return;
    }
    s_lang_options.reserve(kLangCount * 18);
    for (size_t i = 0; i < kLangCount; ++i) {
        if (i > 0) {
            s_lang_options.push_back('\n');
        }
        s_lang_options.append(kLangs[i].name_zh);
        s_lang_options.append(" (");
        s_lang_options.append(kLangs[i].code);
        s_lang_options.push_back(')');
    }
}

int selected_lang_index(lv_obj_t* dd, int fallback) {
    if (dd == nullptr) {
        return fallback;
    }
    const uint32_t sel = lv_dropdown_get_selected(dd);
    if (sel >= kLangCount) {
        return fallback;
    }
    return static_cast<int>(sel);
}

const char* short_lang_name(int idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= kLangCount) {
        return "--";
    }
    // HTML：中文 / English；其余用语种中文名
    if (std::strcmp(kLangs[idx].code, "en") == 0) {
        return "English";
    }
    return kLangs[idx].name_zh;
}

void sync_lang_name_labels() {
    if (s_from_name_lbl != nullptr) {
        lv_label_set_text(s_from_name_lbl,
                          short_lang_name(selected_lang_index(
                              s_from_dd, kDefaultFromIdx)));
    }
    if (s_to_name_lbl != nullptr) {
        lv_label_set_text(s_to_name_lbl,
                          short_lang_name(selected_lang_index(
                              s_to_dd, kDefaultToIdx)));
    }
}

void post_status(const char* text, uint32_t session) {
    if (text == nullptr || !session_alive(session)) {
        return;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (session_alive(session) && screen_alive() && s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, text);
    }
    esp_lv_adapter_unlock();
}

void refresh_result_ui_locked() {
    if (s_source_lbl == nullptr || s_trans_lbl == nullptr) {
        return;
    }
    std::string source;
    std::string trans;
    {
        std::lock_guard<std::mutex> lock(s_text_mutex);
        source = s_source_text;
        trans = s_trans_text;
    }
    if (source.empty()) {
        lv_label_set_text(s_source_lbl, I18n::T("识别原文将显示在这里"));
        lv_obj_set_style_text_color(s_source_lbl, lv_color_hex(kColorHintText),
                                    LV_PART_MAIN);
    } else {
        lv_label_set_text(s_source_lbl, source.c_str());
        lv_obj_set_style_text_color(s_source_lbl, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
    }
    if (trans.empty()) {
        lv_label_set_text(s_trans_lbl, I18n::T("译文将显示在这里"));
        lv_obj_set_style_text_color(s_trans_lbl, lv_color_hex(kColorHintText),
                                    LV_PART_MAIN);
    } else {
        lv_label_set_text(s_trans_lbl, trans.c_str());
        lv_obj_set_style_text_color(s_trans_lbl, lv_color_hex(kColorTrans),
                                    LV_PART_MAIN);
    }
}

void post_result_refresh(uint32_t session) {
    if (!session_alive(session)) {
        return;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (session_alive(session) && screen_alive()) {
        refresh_result_ui_locked();
    }
    esp_lv_adapter_unlock();
}

void clear_results_locked() {
    std::lock_guard<std::mutex> lock(s_text_mutex);
    s_source_text.clear();
    s_trans_text.clear();
}

void apply_recognition(const char* result, const char* trans) {
    bool wrote_source = false;
    bool wrote_trans = false;
    size_t source_len = 0;
    size_t trans_len = 0;
    {
        std::lock_guard<std::mutex> lock(s_text_mutex);
        // 识别原文覆盖显示（不追加历史句）
        if (result != nullptr && result[0] != '\0') {
            s_source_text = result;
            wrote_source = true;
        }
        if (trans != nullptr && trans[0] != '\0') {
            s_trans_text = trans;
            wrote_trans = true;
        }
        source_len = s_source_text.size();
        trans_len = s_trans_text.size();
    }
    ESP_LOGI(TAG,
             "apply source_written=%d trans_written=%d source_len=%u trans_len=%u",
             wrote_source ? 1 : 0, wrote_trans ? 1 : 0, (unsigned)source_len,
             (unsigned)trans_len);
}

void update_action_ui_locked(State st) {
    if (s_action_btn == nullptr || s_action_lbl == nullptr) {
        return;
    }
    switch (st) {
        case State::Idle:
            lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kColorBtnIdle),
                                      LV_PART_MAIN);
            lv_label_set_text(s_action_lbl, I18n::T("按住说话开始翻译"));
            lv_obj_add_flag(s_action_btn, LV_OBJ_FLAG_CLICKABLE);
            if (s_from_dd != nullptr) {
                lv_obj_remove_state(s_from_dd, LV_STATE_DISABLED);
            }
            if (s_to_dd != nullptr) {
                lv_obj_remove_state(s_to_dd, LV_STATE_DISABLED);
            }
            break;
        case State::Connecting:
            lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kColorBtnBusy),
                                      LV_PART_MAIN);
            lv_label_set_text(s_action_lbl, I18n::T("连接中…"));
            lv_obj_add_flag(s_action_btn, LV_OBJ_FLAG_CLICKABLE);
            if (s_from_dd != nullptr) {
                lv_obj_add_state(s_from_dd, LV_STATE_DISABLED);
            }
            if (s_to_dd != nullptr) {
                lv_obj_add_state(s_to_dd, LV_STATE_DISABLED);
            }
            break;
        case State::Streaming:
            lv_obj_set_style_bg_color(s_action_btn,
                                      lv_color_hex(kColorBtnActive),
                                      LV_PART_MAIN);
            lv_label_set_text(s_action_lbl, I18n::T("松开结束"));
            lv_obj_add_flag(s_action_btn, LV_OBJ_FLAG_CLICKABLE);
            break;
        case State::Stopping:
            lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kColorBtnBusy),
                                      LV_PART_MAIN);
            lv_label_set_text(s_action_lbl, I18n::T("停止中…"));
            lv_obj_remove_flag(s_action_btn, LV_OBJ_FLAG_CLICKABLE);
            break;
        case State::Closing:
            break;
    }
}

void post_state(State st, uint32_t session) {
    s_state.store(st, std::memory_order_release);
    if (!session_alive(session) && st != State::Closing) {
        return;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (session == s_session.load(std::memory_order_acquire) &&
        screen_alive()) {
        update_action_ui_locked(st);
    }
    esp_lv_adapter_unlock();
}

void close_websocket() {
    std::lock_guard<std::mutex> lock(s_ws_mutex);
    if (s_ws) {
        s_ws->Close();
        s_ws.reset();
    }
    s_can_send_audio.store(false, std::memory_order_release);
}

struct HttpJsonResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string err;
};

std::unique_ptr<Http> create_http_client(std::string& err_out) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = "no network";
        return nullptr;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        err_out = "create http failed";
        return nullptr;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Connection", "close");
    http->SetHeader("X-Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
    return http;
}

HttpJsonResult http_post_json(const std::string& url, std::string json) {
    HttpJsonResult r;
    if (url.empty()) {
        r.err = "服务地址未配置";
        return r;
    }
    std::lock_guard<std::mutex> lock(s_http_mutex);
    auto http = create_http_client(r.err);
    if (http == nullptr) {
        return r;
    }
    http->SetHeader("Content-Type", "application/json");
    api::LogHttpRequest(TAG, "POST", url, json);
    http->SetContent(std::move(json));
    if (!http->Open("POST", url)) {
        r.err = "open failed";
        api::LogHttpResponse(TAG, -1, r.err);
        return r;
    }
    r.status = http->GetStatusCode();
    r.body = http->ReadAll();
    api::LogHttpResponse(TAG, r.status, r.body);
    http->Close();
    if (r.status != 200) {
        r.err = r.body.empty() ? ("HTTP " + std::to_string(r.status)) : r.body;
        return r;
    }
    r.ok = true;
    return r;
}

bool fetch_ws_url(int lanid, const char* fromlan, const char* tolan,
                  std::string& ws_url_out, std::string& err_out) {
    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "userId", device_user_id().c_str());
    char lanid_buf[16];
    std::snprintf(lanid_buf, sizeof(lanid_buf), "%d", lanid);
    cJSON_AddStringToObject(req, "lanid", lanid_buf);
    cJSON_AddStringToObject(req, "lid", "1");
    cJSON_AddStringToObject(req, "fromlan", fromlan);
    cJSON_AddStringToObject(req, "tolan", tolan);
    char* raw = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (raw == nullptr) {
        err_out = "json alloc failed";
        return false;
    }
    std::string body(raw);
    cJSON_free(raw);

    auto resp = http_post_json(api::Url(api::kSinicloudToken), std::move(body));
    if (!resp.ok) {
        err_out = resp.err.empty() ? "token request failed" : resp.err;
        return false;
    }

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (root == nullptr) {
        err_out = "token json parse failed";
        return false;
    }
    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        err_out = cJSON_IsString(msg) && msg->valuestring
                      ? msg->valuestring
                      : "token api error";
        cJSON_Delete(root);
        return false;
    }
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON* ws = data != nullptr
                    ? cJSON_GetObjectItemCaseSensitive(data, "wsUrl")
                    : nullptr;
    if (!cJSON_IsString(ws) || ws->valuestring == nullptr ||
        ws->valuestring[0] == '\0') {
        err_out = "wsUrl missing";
        cJSON_Delete(root);
        return false;
    }
    ws_url_out = ws->valuestring;
    cJSON_Delete(root);
    return true;
}

void copy_log_snippet(char* out, size_t out_size, const char* src, size_t len) {
    if (out_size == 0) {
        return;
    }
    if (src == nullptr || len == 0) {
        out[0] = '\0';
        return;
    }
    size_t n = len;
    if (n >= out_size) {
        n = out_size - 1;
    }
    std::memcpy(out, src, n);
    out[n] = '\0';
}

void handle_server_json(const char* data, size_t len, uint32_t session) {
    if (data == nullptr || len == 0) {
        ESP_LOGW(TAG, "drop json empty");
        return;
    }
    if (!session_alive(session)) {
        ESP_LOGW(TAG, "drop json session dead len=%u", (unsigned)len);
        return;
    }
    std::string payload(data, len);
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) {
        char snippet[161];
        copy_log_snippet(snippet, sizeof(snippet), payload.c_str(), payload.size());
        ESP_LOGW(TAG, "Network operation failed");
        return;
    }

    cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "errCode");
    std::string code;
    if (cJSON_IsString(err) && err->valuestring != nullptr) {
        code = err->valuestring;
    } else if (cJSON_IsNumber(err)) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", err->valueint);
        code = buf;
    }

    cJSON* result = cJSON_GetObjectItemCaseSensitive(root, "result");
    cJSON* trans = cJSON_GetObjectItemCaseSensitive(root, "trans");
    const char* result_s =
        cJSON_IsString(result) && result->valuestring ? result->valuestring
                                                      : nullptr;
    const char* trans_s =
        cJSON_IsString(trans) && trans->valuestring ? trans->valuestring
                                                    : nullptr;

    char result_snip[161];
    char trans_snip[161];
    const size_t result_len =
        result_s != nullptr ? std::strlen(result_s) : 0;
    const size_t trans_len = trans_s != nullptr ? std::strlen(trans_s) : 0;
    copy_log_snippet(result_snip, sizeof(result_snip), result_s, result_len);
    copy_log_snippet(trans_snip, sizeof(trans_snip), trans_s, trans_len);

    if (code == "1000") {
        s_can_send_audio.store(true, std::memory_order_release);
        post_state(State::Streaming, session);
        post_status(I18n::T("正在录音…松开结束"), session);
        cJSON_Delete(root);
        return;
    }
    if (code == "1001") {
        ESP_LOGW(TAG, "low balance warning");
        cJSON_Delete(root);
        return;
    }
    if (code == "3") {
        apply_recognition(result_s, trans_s);
        post_result_refresh(session);
        cJSON_Delete(root);
        return;
    }
    if (code == "0") {
        apply_recognition(result_s, trans_s);
        post_result_refresh(session);
        s_got_final_result.store(true, std::memory_order_release);
        cJSON_Delete(root);
        return;
    }
    if (code == "10") {
        const bool has_payload =
            (result_s != nullptr && result_s[0] != '\0') ||
            (trans_s != nullptr && trans_s[0] != '\0');
        if (has_payload) {
            // Sonicloud 最终结果常以 errCode=10 携带 result/trans；
            // 若 result 比本地中间原文更短，不回退覆盖。
            const char* source_to_apply = result_s;
            if (result_s != nullptr && result_s[0] != '\0') {
                size_t local_source_len = 0;
                {
                    std::lock_guard<std::mutex> lock(s_text_mutex);
                    local_source_len = s_source_text.size();
                }
                if (local_source_len > 0 && result_len < local_source_len) {
                    ESP_LOGI(TAG,
                             "errCode=10 keep longer source local=%u remote=%u",
                             (unsigned)local_source_len, (unsigned)result_len);
                    source_to_apply = nullptr;
                }
            }
            ESP_LOGI(TAG, "errCode=10 final result/trans");
            apply_recognition(source_to_apply, trans_s);
            post_result_refresh(session);
            s_got_final_result.store(true, std::memory_order_release);
            cJSON_Delete(root);
            return;
        }
        bool has_text = false;
        {
            std::lock_guard<std::mutex> lock(s_text_mutex);
            has_text = !s_source_text.empty() || !s_trans_text.empty();
        }
        if (has_text) {
            // 空结束帧：已有识别/译文，不当作失败
            ESP_LOGI(TAG, "errCode=10 empty end, keep existing text");
            s_got_final_result.store(true, std::memory_order_release);
            cJSON_Delete(root);
            return;
        }
        ESP_LOGW(TAG, "errCode=10 no speech");
        post_status(I18n::T("未识别到语音"), session);
        s_got_session_error.store(true, std::memory_order_release);
        cJSON_Delete(root);
        return;
    }
    if (is_fatal_code(code.c_str())) {
        ESP_LOGE(TAG, "Network operation failed");
        post_status(I18n::T("翻译出错"), session);
        s_got_session_error.store(true, std::memory_order_release);
        s_stop_requested.store(true, std::memory_order_release);
        cJSON_Delete(root);
        return;
    }
    if (result_s != nullptr && result_s[0] != '\0') {
        apply_recognition(result_s, trans_s);
        post_result_refresh(session);
        s_got_final_result.store(true, std::memory_order_release);
    } else {
    }
    cJSON_Delete(root);
}

bool connect_websocket(const std::string& ws_url, uint32_t session,
                       std::string& err_out) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err_out = "no network";
        return false;
    }
    std::shared_ptr<WebSocket> ws(network->CreateWebSocket(1));
    if (!ws) {
        err_out = "create websocket failed";
        return false;
    }
    ws->SetReceiveBufferSize(4096);
    ws->OnData([session](const char* data, size_t len, bool binary) {
        if (binary) {
            ESP_LOGI(TAG, "drop frame binary=1 len=%u", (unsigned)len);
            return;
        }
        if (!session_alive(session)) {
            ESP_LOGW(TAG, "drop frame session dead len=%u", (unsigned)len);
            return;
        }
        char snippet[161];
        copy_log_snippet(snippet, sizeof(snippet), data, len);
        handle_server_json(data, len, session);
    });
    ws->OnDisconnected([session]() {
        ESP_LOGI(TAG, "websocket disconnected");
        s_can_send_audio.store(false, std::memory_order_release);
        if (session_alive(session) &&
            s_state.load(std::memory_order_acquire) == State::Streaming) {
            s_stop_requested.store(true, std::memory_order_release);
        }
    });
    ws->OnError([session](int err) {
        ESP_LOGE(TAG, "websocket error=%d", err);
        if (session_alive(session)) {
            s_stop_requested.store(true, std::memory_order_release);
        }
    });

    // 先挂到 s_ws，再 Connect：退屏硬取消可 Close 打断；本地 shared_ptr 保证 Connect 期间对象存活
    {
        std::lock_guard<std::mutex> lock(s_ws_mutex);
        if (s_ws) {
            s_ws->Close();
            s_ws.reset();
        }
        s_ws = ws;
    }

    ESP_LOGI(TAG, "connecting ws…");
    if (!ws->Connect(ws_url.c_str())) {
        // 仅硬取消 / session 失效视为 cancelled；松手不打断建连
        const bool cancelled =
            s_stop_requested.load(std::memory_order_acquire) ||
            !session_alive(session);
        const int last_err = ws->GetLastError();
        if (cancelled) {
            err_out = "websocket connect cancelled";
            ESP_LOGI(TAG, "ws connect cancelled last_error=%d", last_err);
        } else {
            err_out = "websocket connect failed";
            ESP_LOGE(TAG, "Network operation failed");
        }
        close_websocket();
        return false;
    }

    if (s_stop_requested.load(std::memory_order_acquire) ||
        !session_alive(session)) {
        err_out = "websocket connect cancelled";
        close_websocket();
        return false;
    }
    ESP_LOGI(TAG, "ws connected");
    return true;
}

bool send_pcm_frame(const int16_t* samples, size_t count) {
    if (samples == nullptr || count == 0) {
        return false;
    }
    const size_t bytes = count * sizeof(int16_t);
    if (bytes <= static_cast<size_t>(kMinPcmBytes)) {
        return true;
    }
    std::lock_guard<std::mutex> lock(s_ws_mutex);
    if (!s_ws || !s_ws->IsConnected()) {
        return false;
    }
    return s_ws->Send(samples, bytes, true);
}

void send_end_and_close() {
    s_got_final_result.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(s_ws_mutex);
        if (s_ws && s_ws->IsConnected()) {
            s_ws->Send(std::string("end"));
            ESP_LOGI(TAG, "sent end");
        }
    }
    constexpr int kSliceMs = 50;
    const int slices = kEndWaitMs / kSliceMs;
    const char* reason = "timeout";
    int waited_ms = 0;
    for (int i = 0; i < slices; ++i) {
        if (s_stop_requested.load(std::memory_order_acquire)) {
            reason = "stop";
            break;
        }
        if (s_got_final_result.load(std::memory_order_acquire)) {
            reason = "final";
            break;
        }
        if (s_got_session_error.load(std::memory_order_acquire)) {
            reason = "error";
            break;
        }
        {
            std::lock_guard<std::mutex> lock(s_ws_mutex);
            if (!s_ws || !s_ws->IsConnected()) {
                reason = "disconnected";
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kSliceMs));
        waited_ms += kSliceMs;
    }
    bool source_empty = true;
    bool trans_empty = true;
    {
        std::lock_guard<std::mutex> lock(s_text_mutex);
        source_empty = s_source_text.empty();
        trans_empty = s_trans_text.empty();
    }
    ESP_LOGI(TAG,
             "end wait reason=%s waited_ms=%d source_empty=%d trans_empty=%d",
             reason, waited_ms, source_empty ? 1 : 0, trans_empty ? 1 : 0);
    close_websocket();
}

void stream_pcm_loop(uint32_t session) {
    auto& as = Application::GetInstance().GetAudioService();
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();

    std::vector<int16_t> frame;
    frame.reserve(static_cast<size_t>(kSamplesPerFrame) * 2);

    while (!s_stop_requested.load(std::memory_order_acquire) &&
           !s_release_requested.load(std::memory_order_acquire) &&
           session_alive(session)) {
        if (!s_can_send_audio.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!as.ReadAudioData(frame, kSampleRate, kSamplesPerFrame)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (codec != nullptr && codec->input_channels() == 2) {
            keep_left_channel_only(frame);
        }
        if (!send_pcm_frame(frame.data(), frame.size())) {
            ESP_LOGW(TAG, "send pcm failed");
            break;
        }
    }
}

void worker_task(void* /*arg*/) {
    const uint32_t session = s_session.load(std::memory_order_acquire);
    int from_idx = kDefaultFromIdx;
    int to_idx = kDefaultToIdx;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        from_idx = selected_lang_index(s_from_dd, kDefaultFromIdx);
        to_idx = selected_lang_index(s_to_dd, kDefaultToIdx);
        esp_lv_adapter_unlock();
    }

    bool post_idle = true;

    if (from_idx == to_idx) {
        post_status(I18n::T("源语言与目标语言不能相同"), session);
    } else {
        auto& as = Application::GetInstance().GetAudioService();
        if (!as.IsStarted()) {
            post_status(I18n::T("音频未就绪"), session);
        } else if (as.IsAudioProcessorRunning()) {
            post_status(I18n::T("音频忙，稍后再试"), session);
        } else {
            // 覆盖 token→建连→推流全流程，避免与 AudioInputTask 抢麦；
            // 须在函数正常返回前析构（不可在中途 vTaskDelete）。
            WakeWordGuard wake_guard(as);

            clear_results_locked();
            post_result_refresh(session);
            post_status(I18n::T("获取令牌…"), session);

            // 尽早开始预缓冲，覆盖 4G HTTP/WS 建连耗时
            start_capture_task(session);

            const LangEntry& from = kLangs[from_idx];
            const LangEntry& to = kLangs[to_idx];
            std::string ws_url;
            std::string err;
            if (!fetch_ws_url(from.lanid, from.code, to.code, ws_url, err)) {
                ESP_LOGE(TAG, "Network operation failed");
                post_status(I18n::T("获取令牌失败"), session);
            } else if (s_stop_requested.load(std::memory_order_acquire) ||
                       !session_alive(session)) {
                post_idle = false;
            } else {
                post_status(I18n::T("连接中…"), session);
                if (!connect_websocket(ws_url, session, err)) {
                    if (err == "websocket connect cancelled" ||
                        s_stop_requested.load(std::memory_order_acquire) ||
                        !session_alive(session)) {
                        ESP_LOGI(TAG, "ws connect cancelled");
                        if (session_alive(session)) {
                            post_status(I18n::T("已停止"), session);
                        } else {
                            post_idle = false;
                        }
                    } else {
                        ESP_LOGE(TAG, "Network operation failed");
                        if (session_alive(session)) {
                            post_status(I18n::T("连接失败"), session);
                        } else {
                            post_idle = false;
                        }
                    }
                } else {
                    // 等待 errCode=1000；松手不中断等待，硬取消才退出
                    for (int i = 0; i < 100 && session_alive(session) &&
                                    !s_stop_requested.load(
                                        std::memory_order_acquire);
                         ++i) {
                        if (s_can_send_audio.load(std::memory_order_acquire)) {
                            break;
                        }
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }

                    if (!s_can_send_audio.load(std::memory_order_acquire)) {
                        post_status(I18n::T("等待就绪超时"), session);
                        close_websocket();
                    } else if (s_stop_requested.load(std::memory_order_acquire) ||
                               !session_alive(session)) {
                        close_websocket();
                        post_idle = false;
                    } else {
                        post_state(State::Streaming, session);
                        post_status(I18n::T("正在录音…松开结束"), session);
                        // 冲刷期间不停麦：4G 发完预缓冲约需数秒，停麦会造成断档
                        bool flush_ok = prebuffer_flush_send();
                        // 排空冲刷窗口内续采的残余；松手/停止则退出（4G 满缓冲时否则会死循环）
                        while (flush_ok && session_alive(session) &&
                               !s_stop_requested.load(
                                   std::memory_order_acquire) &&
                               !s_release_requested.load(
                                   std::memory_order_acquire)) {
                            size_t pending = prebuffer_pending_count();
                            if (pending == 0) {
                                vTaskDelay(pdMS_TO_TICKS(20));
                                pending = prebuffer_pending_count();
                            }
                            if (pending == 0) {
                                break;
                            }
                            ESP_LOGI(TAG, "flush residual samples=%u",
                                     (unsigned)pending);
                            flush_ok = prebuffer_flush_send();
                        }
                        stop_capture_task();
                        if (flush_ok && prebuffer_pending_count() > 0) {
                            ESP_LOGI(TAG, "flush residual samples=%u",
                                     (unsigned)prebuffer_pending_count());
                            flush_ok = prebuffer_flush_send();
                        }
                        if (!flush_ok) {
                            post_status(I18n::T("连接失败"), session);
                            close_websocket();
                        } else {
                            const bool already_released =
                                s_release_requested.load(
                                    std::memory_order_acquire);
                            if (!already_released) {
                                stream_pcm_loop(session);
                            }

                            if (session_alive(session)) {
                                post_state(State::Stopping, session);
                                post_status(I18n::T("停止中…"), session);
                            }
                            send_end_and_close();

                            if (session_alive(session)) {
                                bool has_text = false;
                                {
                                    std::lock_guard<std::mutex> lock(s_text_mutex);
                                    has_text = !s_source_text.empty() ||
                                               !s_trans_text.empty();
                                }
                                if (!s_got_session_error.load(
                                        std::memory_order_acquire)) {
                                    if (has_text) {
                                        post_status(I18n::T("已停止"), session);
                                    } else {
                                        post_status(I18n::T("未识别到语音"),
                                                    session);
                                    }
                                }
                                post_state(State::Idle, session);
                                post_idle = false;
                            } else {
                                post_idle = false;
                            }
                        }
                    }
                }
            }

            stop_capture_task();
        }
    }

    if (post_idle && session_alive(session)) {
        post_state(State::Idle, session);
    }
    s_worker_task = nullptr;
    vTaskDeleteWithCaps(nullptr);
}

void start_session() {
    if (s_worker_task != nullptr) {
        return;
    }
    State st = s_state.load(std::memory_order_acquire);
    if (st != State::Idle) {
        return;
    }
    s_stop_requested.store(false, std::memory_order_release);
    s_release_requested.store(false, std::memory_order_release);
    s_can_send_audio.store(false, std::memory_order_release);
    s_got_final_result.store(false, std::memory_order_release);
    s_got_session_error.store(false, std::memory_order_release);
    post_state(State::Connecting, s_session.load(std::memory_order_acquire));

    // 8KB 栈从内部 DRAM 分配易因碎片化失败；改 PSRAM（与音频/AI 绘图 worker 一致）。
    constexpr UBaseType_t kWorkerStackCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        worker_task, "translate_ws", 8192, nullptr, 5, &s_worker_task, 0,
        kWorkerStackCaps);
    if (ok != pdPASS) {
        s_worker_task = nullptr;
        ESP_LOGE(TAG,
                 "create worker failed (internal=%uKB largest=%uKB psram=%uKB)",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) /
                            1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        post_status(I18n::T("启动失败"), s_session.load());
        post_state(State::Idle, s_session.load());
    }
}

void on_action_pressed(lv_event_t* /*e*/) {
    if (s_state.load(std::memory_order_acquire) == State::Idle) {
        start_session();
    }
}

void on_action_released(lv_event_t* /*e*/) {
    const State st = s_state.load(std::memory_order_acquire);
    if (st != State::Streaming && st != State::Connecting) {
        return;
    }
    // 松手：Connecting 不打断 MIPSTATE/WS 建连；Streaming 才进入 Stopping
    s_release_requested.store(true, std::memory_order_release);
    if (st == State::Streaming) {
        post_state(State::Stopping, s_session.load(std::memory_order_acquire));
    }
}

void stop_session_for_exit() {
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_state.store(State::Closing, std::memory_order_release);
    s_stop_requested.store(true, std::memory_order_release);
    s_release_requested.store(true, std::memory_order_release);
    s_can_send_audio.store(false, std::memory_order_release);
    s_capturing.store(false, std::memory_order_release);
    close_websocket();
}

void go_home() {
    stop_session_for_exit();
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void on_swipe_back() { go_home(); }

void on_screen_unloaded(lv_event_t* e) {
    lv_obj_t* scr = lv_event_get_target_obj(e);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_state.store(State::Closing, std::memory_order_release);
    s_stop_requested.store(true, std::memory_order_release);
    s_release_requested.store(true, std::memory_order_release);
    s_can_send_audio.store(false, std::memory_order_release);
    s_capturing.store(false, std::memory_order_release);
    close_websocket();

    // 若本屏已被更新的 Create() 替换，静态指针属于新实例，不能在这里清掉。
    if (scr != s_screen) {
        ESP_LOGI(TAG, "unload: stale screen instance ignored");
        return;
    }

    s_screen = nullptr;
    s_from_dd = nullptr;
    s_to_dd = nullptr;
    s_from_name_lbl = nullptr;
    s_to_name_lbl = nullptr;
    s_status_lbl = nullptr;
    s_source_lbl = nullptr;
    s_trans_lbl = nullptr;
    s_action_btn = nullptr;
    s_action_lbl = nullptr;
}

void on_dropdown_ready(lv_event_t* e) {
    lv_obj_t* dd = lv_event_get_target_obj(e);
    if (dd == nullptr) {
        return;
    }
    lv_obj_t* list = lv_dropdown_get_list(dd);
    if (list == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(list, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_text_color(list, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(list, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 10, LV_PART_MAIN);
    lv_obj_set_style_max_height(list, 140, LV_PART_MAIN);
    // 列表默认贴在左右语言卡下方会偏左/偏右；统一拉到屏幕正中
    lv_obj_set_width(list, kPanelW - 36);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 4);
    // LVGL 打开后再改一次坐标，避免被默认布局覆盖
    lv_async_call(
        [](void* user) {
            auto* list_obj = static_cast<lv_obj_t*>(user);
            if (list_obj == nullptr || !lv_obj_is_valid(list_obj)) {
                return;
            }
            lv_obj_set_width(list_obj, kPanelW - 36);
            lv_obj_align(list_obj, LV_ALIGN_CENTER, 0, 4);
        },
        list);
    screen_swipe_back_ignore(list, true);
}

void style_dropdown(lv_obj_t* dd) {
    lv_obj_set_style_radius(dd, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dd, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dd, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(dd, lv_color_hex(0x4B5563), LV_PART_MAIN);
    lv_obj_set_style_text_color(dd, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_dropdown_set_symbol(dd, LV_SYMBOL_DOWN);
    lv_obj_add_event_cb(dd, on_dropdown_ready, LV_EVENT_READY, nullptr);
    screen_swipe_back_ignore(dd, true);
}

void build_header(lv_obj_t* parent) {
    lv_obj_t* top = lv_obj_create(parent);
    screen_strip_obj_chrome(top);
    lv_obj_set_size(top, kPanelW, kHeaderH);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(top, lv_color_hex(kColorHeaderBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* divider = lv_obj_create(top);
    screen_strip_obj_chrome(divider);
    lv_obj_set_size(divider, kPanelW, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorDivider),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    screen_make_input_passive(divider);

    lv_obj_t* title = lv_label_create(top);
    lv_label_set_text(title, I18n::T("翻译"));
    lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    screen_make_input_passive(title);
}

void build_body(lv_obj_t* parent) {
    build_lang_options();

    lv_obj_t* body = lv_obj_create(parent);
    screen_strip_obj_chrome(body);
    lv_obj_set_size(body, kPanelW, kBodyH);
    lv_obj_set_pos(body, 0, kHeaderH);
    lv_obj_set_style_bg_color(body, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(body, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_right(body, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_top(body, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(body, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 6, LV_PART_MAIN);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    // 源语言 | ⇄ | 目标语言（对齐 HTML lang-bar）
    lv_obj_t* lang_bar = lv_obj_create(body);
    screen_strip_obj_chrome(lang_bar);
    lv_obj_set_size(lang_bar, LV_PCT(100), 46);
    lv_obj_set_style_bg_opa(lang_bar, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(lang_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lang_bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(lang_bar, 6, LV_PART_MAIN);
    lv_obj_remove_flag(lang_bar, LV_OBJ_FLAG_SCROLLABLE);

    auto make_lang_card = [&](lv_obj_t** out_dd, lv_obj_t** out_name,
                              int default_idx, const char* caption) {
        lv_obj_t* card = lv_obj_create(lang_bar);
        screen_strip_obj_chrome(card);
        lv_obj_set_height(card, LV_PCT(100));
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 4, LV_PART_MAIN);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(card, 1, LV_PART_MAIN);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        screen_swipe_back_ignore(card, true);

        lv_obj_t* cap = lv_label_create(card);
        lv_label_set_text(cap, caption);
        lv_obj_set_style_text_font(cap, &font_puhui_16_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(cap, lv_color_hex(kColorHintText),
                                    LV_PART_MAIN);
        screen_make_input_passive(cap);

        lv_obj_t* name = lv_label_create(card);
        *out_name = name;
        lv_label_set_text(name, short_lang_name(default_idx));
        lv_obj_set_style_text_font(name, &font_puhui_16_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(name, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        screen_make_input_passive(name);

        // 下拉藏在卡内，点卡打开（保留完整语种表）
        lv_obj_t* dd = lv_dropdown_create(card);
        *out_dd = dd;
        lv_obj_set_width(dd, LV_PCT(100));
        lv_obj_set_height(dd, 1);
        style_dropdown(dd);
        lv_dropdown_set_options(dd, s_lang_options.c_str());
        lv_dropdown_set_selected(dd, default_idx);
        lv_obj_add_flag(dd, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(
            dd,
            [](lv_event_t* /*e*/) { sync_lang_name_labels(); },
            LV_EVENT_VALUE_CHANGED, nullptr);

        lv_obj_add_event_cb(
            card,
            [](lv_event_t* e) {
                lv_obj_t* dd_obj = static_cast<lv_obj_t*>(
                    lv_event_get_user_data(e));
                if (dd_obj == nullptr) {
                    return;
                }
                if (lv_obj_has_state(dd_obj, LV_STATE_DISABLED)) {
                    return;
                }
                lv_dropdown_open(dd_obj);
            },
            LV_EVENT_CLICKED, dd);
        return card;
    };

    make_lang_card(&s_from_dd, &s_from_name_lbl, kDefaultFromIdx,
                   I18n::T("源语言"));

    lv_obj_t* swap = lv_button_create(lang_bar);
    lv_obj_remove_style_all(swap);
    lv_obj_set_size(swap, 28, 28);
    lv_obj_set_style_radius(swap, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(swap, lv_color_hex(0x151922), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(swap, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(swap, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(swap, lv_color_hex(kColorDivider),
                                  LV_PART_MAIN);
    screen_swipe_back_ignore(swap, true);
    lv_obj_add_event_cb(
        swap,
        [](lv_event_t* /*e*/) {
            if (s_from_dd == nullptr || s_to_dd == nullptr) {
                return;
            }
            const uint32_t a = lv_dropdown_get_selected(s_from_dd);
            const uint32_t b = lv_dropdown_get_selected(s_to_dd);
            lv_dropdown_set_selected(s_from_dd, b);
            lv_dropdown_set_selected(s_to_dd, a);
            sync_lang_name_labels();
        },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_t* swap_lbl = lv_label_create(swap);
    lv_label_set_text(swap_lbl, LV_SYMBOL_LOOP);
    lv_obj_set_style_text_color(swap_lbl, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_center(swap_lbl);

    make_lang_card(&s_to_dd, &s_to_name_lbl, kDefaultToIdx,
                   I18n::T("目标语言"));
    sync_lang_name_labels();

    // 单一结果卡：原文 + 译文
    lv_obj_t* result = lv_obj_create(body);
    screen_strip_obj_chrome(result);
    lv_obj_set_width(result, LV_PCT(100));
    lv_obj_set_flex_grow(result, 1);
    lv_obj_set_style_bg_color(result, lv_color_hex(0x151922), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(result, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(result, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(result, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(result, lv_color_hex(kColorDivider),
                                  LV_PART_MAIN);
    lv_obj_set_style_pad_all(result, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(result, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(result, 6, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(result, LV_SCROLLBAR_MODE_AUTO);
    screen_swipe_back_ignore(result, true);

    s_source_lbl = lv_label_create(result);
    lv_obj_set_width(s_source_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_source_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_source_lbl, &font_puhui_16_4, LV_PART_MAIN);
    screen_make_input_passive(s_source_lbl);

    s_trans_lbl = lv_label_create(result);
    lv_obj_set_width(s_trans_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_trans_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_trans_lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_trans_lbl, lv_color_hex(kColorTrans),
                                LV_PART_MAIN);
    screen_make_input_passive(s_trans_lbl);

    s_status_lbl = lv_label_create(body);
    lv_obj_set_width(s_status_lbl, LV_PCT(100));
    lv_obj_set_height(s_status_lbl, kStatusH);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_font(s_status_lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    screen_make_input_passive(s_status_lbl);

    refresh_result_ui_locked();
}

void build_footer(lv_obj_t* parent) {
    lv_obj_t* footer = lv_obj_create(parent);
    screen_strip_obj_chrome(footer);
    lv_obj_set_size(footer, kPanelW, kFooterH);
    lv_obj_set_pos(footer, 0, kPanelH - kFooterH);
    lv_obj_set_style_bg_color(footer, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    constexpr int32_t kBtnW = kPanelW - 32;
    constexpr int32_t kBtnH = 28;
    s_action_btn = lv_button_create(footer);
    lv_obj_set_size(s_action_btn, kBtnW, kBtnH);
    lv_obj_align(s_action_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_action_btn, kBtnH / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_action_btn, lv_color_hex(kColorBtnIdle),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_action_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_action_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_action_btn, 0, LV_PART_MAIN);

    s_action_lbl = lv_label_create(s_action_btn);
    lv_label_set_text(s_action_lbl, I18n::T("按住说话开始翻译"));
    lv_obj_set_style_text_color(s_action_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_action_lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_center(s_action_lbl);

    lv_obj_add_event_cb(s_action_btn, on_action_pressed, LV_EVENT_PRESSED,
                        nullptr);
    lv_obj_add_event_cb(s_action_btn, on_action_released, LV_EVENT_RELEASED,
                        nullptr);
    lv_obj_add_event_cb(s_action_btn, on_action_released,
                        LV_EVENT_PRESS_LOST, nullptr);
    screen_swipe_back_ignore(s_action_btn, true);
}

}  // namespace

lv_obj_t* TranslateScreen::Create() {
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_state.store(State::Idle, std::memory_order_release);
    s_stop_requested.store(false, std::memory_order_release);
    s_release_requested.store(false, std::memory_order_release);
    s_can_send_audio.store(false, std::memory_order_release);
    s_got_final_result.store(false, std::memory_order_release);
    s_got_session_error.store(false, std::memory_order_release);
    clear_results_locked();

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_header(scr);
    build_body(scr);
    build_footer(scr);

    lv_obj_add_event_cb(scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    screen_attach_swipe_back(scr, on_swipe_back);
    return scr;
}

void TranslateScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: translate_screen");
        return;
    }
    ESP_LOGI(TAG, "unload: translate_screen");
    s_state.store(State::Closing, std::memory_order_release);
    s_stop_requested.store(true, std::memory_order_release);
    s_release_requested.store(true, std::memory_order_release);
    s_capturing.store(false, std::memory_order_release);
    close_websocket();
    // 翻译页不持有语音 UI 会话；只确保不误开唤醒词。
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
}
