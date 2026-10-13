#include "ai_image_gen_screen.h"
#include "i18n.h"

#include <atomic>
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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "api_endpoints.h"
#include "application.h"
#include "audio_codec.h"
#include "audio_service.h"
#include "board.h"
#include "device_state.h"
#include "home_screen/home_screen.h"
#include "http.h"
#include "lvgl_image.h"
#include "panel_metrics.h"
#include "screen_util.h"
#include "system_info.h"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);

namespace {

constexpr const char* TAG = "AiImageGenScreen";

constexpr int  kSampleRate       = 16000;
constexpr int  kSamplesPerFrame  = kSampleRate * 60 / 1000;
constexpr int  kBytesPerSample   = 2;
constexpr int  kMaxRecordSecondsWifi = 30;
// 4G 模组 HTTP 上传能力有限；过长 WAV 易触发 MHTTPCONTENT CME/超时
constexpr int  kMaxRecordSecondsCellular = 8;
constexpr size_t kMaxRecordBytesWifi =
    static_cast<size_t>(kSampleRate) * kBytesPerSample * kMaxRecordSecondsWifi;
constexpr size_t kMaxRecordBytesCellular =
    static_cast<size_t>(kSampleRate) * kBytesPerSample *
    kMaxRecordSecondsCellular;
constexpr int  kMinRecordMs      = 300;
constexpr size_t kBodyLogPrefixLen = 256;

bool is_cellular_network() {
    return Board::GetInstance().GetBoardType() == "ml307";
}

void clear_own_worker_handle();

[[noreturn]] void abort_worker_session_dead(const char* stage) {
    ESP_LOGW(TAG, "worker abort: session dead at %s", stage);
    clear_own_worker_handle();
    vTaskDeleteWithCaps(nullptr);
}

constexpr int32_t kPanelW      = panel::kWidth;
constexpr int32_t kPanelH      = panel::kHeight;
constexpr int32_t kHeaderH     = panel::kHeaderH;
constexpr int32_t kTabBarH     = panel::kTabBarH;
constexpr int32_t kFooterH     = 44;
constexpr int32_t kBodyH       = kPanelH - kHeaderH - kFooterH;
constexpr int32_t kHeaderSidePad = 4;
constexpr int32_t kPromptAreaH = 36;

constexpr int   kPollIntervalMs      = 3000;
constexpr int   kPollFirstDelayMs    = 1500;
constexpr int   kMaxPollAttempts     = 60;
constexpr int   kMaxPollHttpFails    = 8;
constexpr int   kHttpTimeoutMs       = 30000;
// 4G 模组拉图走 AT URC，大图/chunked 很慢；下载单独加长超时
constexpr int   kImageDownloadTimeoutMs = 90000;
// 240 屏足够；1280 图体会过大，易触发 Ml307Http content 超时
constexpr const char* kText2ImageSize = "512*512";
constexpr size_t kImageMaxBytes      = 400 * 1024;
constexpr size_t kMaxButtonTextBytes = 96;

constexpr uint32_t kColorBg         = 0x0E1116;
constexpr uint32_t kColorHeaderBg   = 0x12151C;
constexpr uint32_t kColorDivider    = 0x2A2F3A;
constexpr uint32_t kColorCard       = 0x1A1F2A;
constexpr uint32_t kColorText       = 0xFFFFFF;
constexpr uint32_t kColorHintText   = 0x9AA3B2;
constexpr uint32_t kColorPromptText = 0xE5E7EB;

constexpr uint32_t kColorRecordBtnIdle   = 0x2563EB;
constexpr uint32_t kColorRecordBtnActive = 0xDC2626;
constexpr uint32_t kColorRecordBtnBusy   = 0x4B5563;
constexpr uint32_t kColorTabActive       = 0x3B82F6;

enum class State : uint8_t {
    Idle,
    Recording,
    Busy,  // ASR / 生图 / 下载
    Closing,
};

std::atomic<State>   s_state{State::Idle};
std::atomic<int64_t> s_record_start_us{0};
std::atomic<int64_t> s_gen_start_us{0};  // >0 表示处于「生成中」并显示已用时
std::atomic<bool>    s_stop_requested{false};
std::atomic<uint32_t> s_session{0};
std::mutex           s_http_mutex;

lv_obj_t* s_screen      = nullptr;
lv_obj_t* s_prompt_lbl  = nullptr;
lv_obj_t* s_hint_lbl    = nullptr;
lv_obj_t* s_gallery     = nullptr;
lv_obj_t* s_record_btn  = nullptr;
lv_obj_t* s_record_lbl  = nullptr;
lv_obj_t* s_n_dd        = nullptr;
lv_timer_t* s_tick_timer = nullptr;
TaskHandle_t s_worker_task = nullptr;

std::vector<std::unique_ptr<LvglAllocatedImage>> s_image_holders;

bool screen_alive() { return s_screen != nullptr; }

bool session_alive(uint32_t session) {
    return session == s_session.load(std::memory_order_acquire) &&
           s_state.load(std::memory_order_acquire) != State::Closing;
}

struct HeapCapsDeleter {
    void operator()(void* p) const {
        if (p != nullptr) {
            heap_caps_free(p);
        }
    }
};
using CapsBuffer = std::unique_ptr<uint8_t, HeapCapsDeleter>;

CapsBuffer alloc_caps(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
        p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    }
    return CapsBuffer(static_cast<uint8_t*>(p));
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
        // 仅当语音 UI 会话仍在时恢复，避免生图页把桌面唤醒词误打开。
        if (disabled && Application::GetInstance().IsVoiceUiActive()) {
            as.EnableWakeWordDetection(true);
        }
    }
    WakeWordGuard(const WakeWordGuard&) = delete;
    WakeWordGuard& operator=(const WakeWordGuard&) = delete;
};

const std::string& device_id_header() {
    static const std::string kId = SystemInfo::GetMacAddress();
    return kId;
}

void fill_wav_header(uint8_t* hdr, uint32_t data_bytes) {
    const uint32_t sample_rate = kSampleRate;
    const uint16_t channels = 1;
    const uint16_t bits = 16;
    const uint32_t byte_rate = sample_rate * channels * bits / 8;
    const uint16_t block_align = channels * bits / 8;
    const uint32_t riff_size = 36 + data_bytes;
    auto put32 = [](uint8_t* p, uint32_t v) {
        p[0] = v & 0xFF;
        p[1] = (v >> 8) & 0xFF;
        p[2] = (v >> 16) & 0xFF;
        p[3] = (v >> 24) & 0xFF;
    };
    auto put16 = [](uint8_t* p, uint16_t v) {
        p[0] = v & 0xFF;
        p[1] = (v >> 8) & 0xFF;
    };
    std::memcpy(hdr + 0, "RIFF", 4);
    put32(hdr + 4, riff_size);
    std::memcpy(hdr + 8, "WAVE", 4);
    std::memcpy(hdr + 12, "fmt ", 4);
    put32(hdr + 16, 16);
    put16(hdr + 20, 1);
    put16(hdr + 22, channels);
    put32(hdr + 24, sample_rate);
    put32(hdr + 28, byte_rate);
    put16(hdr + 32, block_align);
    put16(hdr + 34, bits);
    std::memcpy(hdr + 36, "data", 4);
    put32(hdr + 40, data_bytes);
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

int selected_image_count() {
    if (s_n_dd == nullptr) {
        return 1;
    }
    const uint32_t sel = lv_dropdown_get_selected(s_n_dd);
    const int n = static_cast<int>(sel) + 1;
    if (n < 1) {
        return 1;
    }
    if (n > 4) {
        return 4;
    }
    return n;
}

// 状态文案写在底部按钮上（底部不再另放状态行）。
void post_button_text(const char* text, uint32_t session) {
    if (text == nullptr) {
        return;
    }
    if (session != s_session.load(std::memory_order_acquire)) {
        return;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (session == s_session.load(std::memory_order_acquire) && screen_alive() &&
        s_record_lbl != nullptr) {
        // 过长错误体（HTML/JSON）不适合塞进按钮。
        char buf[kMaxButtonTextBytes];
        std::snprintf(buf, sizeof(buf), "%s", text);
        lv_label_set_text(s_record_lbl, buf);
    }
    esp_lv_adapter_unlock();
}

void update_button_ui_locked(State st) {
    if (s_record_btn == nullptr || s_record_lbl == nullptr) {
        return;
    }
    switch (st) {
        case State::Idle:
            lv_obj_set_style_bg_color(s_record_btn,
                                      lv_color_hex(kColorRecordBtnIdle),
                                      LV_PART_MAIN);
            lv_label_set_text(s_record_lbl, I18n::T("按住说话"));
            lv_obj_add_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            if (s_n_dd != nullptr) {
                lv_obj_remove_state(s_n_dd, LV_STATE_DISABLED);
            }
            break;
        case State::Recording:
            lv_obj_set_style_bg_color(s_record_btn,
                                      lv_color_hex(kColorRecordBtnActive),
                                      LV_PART_MAIN);
            lv_label_set_text(s_record_lbl, I18n::T("已录 0.0 秒"));
            lv_obj_add_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            if (s_n_dd != nullptr) {
                lv_obj_add_state(s_n_dd, LV_STATE_DISABLED);
            }
            break;
        case State::Busy:
            lv_obj_set_style_bg_color(s_record_btn,
                                      lv_color_hex(kColorRecordBtnBusy),
                                      LV_PART_MAIN);
            // 文案由 post_button_text / tick_timer 覆盖（识别中 / 生成中已用时）
            lv_obj_remove_flag(s_record_btn, LV_OBJ_FLAG_CLICKABLE);
            if (s_n_dd != nullptr) {
                lv_obj_add_state(s_n_dd, LV_STATE_DISABLED);
            }
            break;
        case State::Closing:
            break;
    }
}

void set_state_ui(State st, uint32_t session) {
    if (session != s_session.load(std::memory_order_acquire)) {
        return;
    }
    s_state.store(st, std::memory_order_release);
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    if (session == s_session.load(std::memory_order_acquire) && screen_alive()) {
        update_button_ui_locked(st);
    }
    esp_lv_adapter_unlock();
}

// 退出 worker：先恢复 Idle（可点），再可选覆盖错误文案。
void clear_own_worker_handle() {
    if (s_worker_task == xTaskGetCurrentTaskHandle()) {
        s_worker_task = nullptr;
    }
}

void finish_worker(uint32_t session, const char* button_text = nullptr) {
    s_gen_start_us.store(0, std::memory_order_release);
    set_state_ui(State::Idle, session);
    if (button_text != nullptr && button_text[0] != '\0') {
        post_button_text(button_text, session);
    }
    clear_own_worker_handle();
}

void clear_gallery_locked() {
    // 先删 LVGL 节点，再释放 image holder，避免短暂悬空引用。
    if (s_gallery != nullptr) {
        while (lv_obj_get_child_count(s_gallery) > 0) {
            lv_obj_delete(lv_obj_get_child(s_gallery, 0));
        }
    }
    s_image_holders.clear();
}

void set_prompt_locked(const std::string& prompt) {
    if (s_prompt_lbl == nullptr) {
        return;
    }
    if (prompt.empty()) {
        lv_label_set_text(s_prompt_lbl, I18n::T("按住说话，说出想画的内容"));
    } else {
        lv_label_set_text(s_prompt_lbl, prompt.c_str());
    }
}

void add_image_widget(lv_obj_t* parent, const LvglAllocatedImage* holder) {
    if (parent == nullptr || holder == nullptr) {
        return;
    }
    const lv_img_dsc_t* dsc = holder->image_dsc();
    lv_obj_t* iv = lv_image_create(parent);
    lv_image_set_src(iv, dsc);

    // 240 屏：按画廊区域铺满（等比放大），避免 JPEG 1:1 缩略图居中显得过小。
    lv_obj_update_layout(parent);
    int32_t box_w = lv_obj_get_content_width(parent);
    int32_t box_h = lv_obj_get_content_height(parent);
    if (box_w < 8) {
        box_w = kPanelW - 24;
    }
    if (box_h < 8) {
        box_h = kBodyH - kPromptAreaH - 28;
    }

    int32_t img_w = (dsc != nullptr) ? dsc->header.w : 0;
    int32_t img_h = (dsc != nullptr) ? dsc->header.h : 0;
    if (img_w > 0 && img_h > 0) {
        const int32_t scale_w = (box_w * 256) / img_w;
        const int32_t scale_h = (box_h * 256) / img_h;
        int32_t scale = scale_w < scale_h ? scale_w : scale_h;
        if (scale < 32) {
            scale = 32;
        }
        if (scale > 1024) {
            scale = 1024;
        }
        lv_image_set_scale(iv, static_cast<uint16_t>(scale));
        lv_obj_set_size(iv, (img_w * scale) / 256, (img_h * scale) / 256);
    } else {
        lv_obj_set_size(iv, box_w, box_h);
        lv_image_set_inner_align(iv, LV_IMAGE_ALIGN_CONTAIN);
    }
    lv_obj_center(iv);
    screen_make_input_passive(iv);
}

lv_obj_t* make_image_card(lv_obj_t* parent, const LvglAllocatedImage* holder) {
    lv_obj_t* wrap = lv_obj_create(parent);
    screen_strip_obj_chrome(wrap);
    lv_obj_set_size(wrap, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(wrap, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(wrap, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wrap, 2, LV_PART_MAIN);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    add_image_widget(wrap, holder);
    return wrap;
}

void tick_timer_cb(lv_timer_t* /*t*/) {
    if (s_record_lbl == nullptr) {
        return;
    }
    const State st = s_state.load(std::memory_order_acquire);
    if (st == State::Recording) {
        const int64_t start = s_record_start_us.load(std::memory_order_acquire);
        if (start <= 0) {
            return;
        }
        const int ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
        char buf[32];
        std::snprintf(buf, sizeof(buf), I18n::T("已录 %d.%d 秒"), ms / 1000,
                      (ms / 100) % 10);
        lv_label_set_text(s_record_lbl, buf);
        return;
    }
    if (st == State::Busy) {
        const int64_t start = s_gen_start_us.load(std::memory_order_acquire);
        if (start <= 0) {
            return;
        }
        const int sec =
            static_cast<int>((esp_timer_get_time() - start) / 1000000);
        char buf[48];
        std::snprintf(buf, sizeof(buf), I18n::T("生成中… 已用时 %d 秒"),
                      sec < 0 ? 0 : sec);
        lv_label_set_text(s_record_lbl, buf);
    }
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
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
    http->SetHeader("Device-Id", device_id_header());
    http->SetHeader("X-Device-Id", device_id_header());
    return http;
}

HttpJsonResult http_exchange(const char* method, const std::string& url,
                             std::string body, const char* content_type) {
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
    if (content_type != nullptr && content_type[0] != '\0') {
        http->SetHeader("Content-Type", content_type);
    }

    // 大二进制走 chunked Write，避免 ML307 在 MHTTPREQUEST 前整包缓存触发 CME 50
    const bool stream_body =
        !body.empty() && content_type != nullptr &&
        std::strcmp(content_type, "application/octet-stream") == 0;

    if (body.empty()) {
        api::LogHttpRequest(TAG, method, url);
    } else if (content_type != nullptr &&
               std::strcmp(content_type, "application/json") == 0) {
        api::LogHttpRequest(TAG, method, url, body);
        http->SetContent(std::move(body));
    } else if (stream_body) {
        api::LogHttpBinaryRequest(TAG, method, url, body.size());
    } else {
        api::LogHttpBinaryRequest(TAG, method, url, body.size());
        http->SetContent(std::move(body));
    }
    const int64_t open_start_us = esp_timer_get_time();
    if (!http->Open(method, url)) {
        r.err = "open failed";
        ESP_LOGE(TAG, "Network operation failed");
        api::LogHttpResponse(TAG, -1, r.err);
        return r;
    }

    if (stream_body) {
        const size_t total = body.size();
        ESP_LOGI(TAG,
                 "http_exchange stream write start len=%u cellular=%d",
                 static_cast<unsigned>(total),
                 is_cellular_network() ? 1 : 0);
        // Ml307Http::Write 按 4KB 切分并返回已写 body 字节；
        // HttpClient chunked 成功时返回含帧头的发送长度（>= body）。
        const int n = http->Write(body.data(), total);
        if (n <= 0 || static_cast<size_t>(n) < total) {
            const int elapsed_ms = static_cast<int>(
                (esp_timer_get_time() - open_start_us) / 1000);
            const unsigned wrote =
                n < 0 ? 0u : static_cast<unsigned>(n);
            const unsigned pct =
                total == 0 ? 0u
                           : static_cast<unsigned>((wrote * 100u) / total);
            r.err = "write failed";
            ESP_LOGE(TAG, "Network operation failed");
            api::LogHttpResponse(TAG, -1, r.err);
            http->Close();
            return r;
        }
        // 结束 chunked 正文（ML307 / WiFi HttpClient 均约定 size=0）
        http->Write("", 0);
        ESP_LOGI(TAG, "http_exchange streamed body len=%u",
                 static_cast<unsigned>(total));
        body.clear();
        body.shrink_to_fit();
    }

    r.status = http->GetStatusCode();
    r.body = http->ReadAll();
    if (std::strcmp(method, "GET") == 0) {
        api::LogHttpResponse(TAG, r.status, api::RedactClawUrlsForLog(r.body));
    } else {
        api::LogHttpResponse(TAG, r.status, r.body);
    }
    http->Close();
    if (r.status != 200) {
        r.err = r.body.empty() ? ("HTTP " + std::to_string(r.status)) : r.body;
        return r;
    }
    r.ok = true;
    return r;
}

HttpJsonResult http_post_binary(const std::string& url, std::string body,
                                const char* content_type) {
    return http_exchange("POST", url, std::move(body), content_type);
}

HttpJsonResult http_post_json(const std::string& url, std::string json) {
    return http_exchange("POST", url, std::move(json), "application/json");
}

HttpJsonResult http_get_json(const std::string& url) {
    return http_exchange("GET", url, std::string(), nullptr);
}

struct BinaryDownload {
    bool ok = false;
    CapsBuffer data;
    size_t size = 0;
    std::string err;
};

bool looks_like_image(const uint8_t* buf, size_t total) {
    if (buf == nullptr || total < 3) {
        return false;
    }
    if (buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF) {
        return true;  // JPEG
    }
    if (total >= 8 && buf[0] == 0x89 && buf[1] == 'P' && buf[2] == 'N' &&
        buf[3] == 'G') {
        return true;  // PNG
    }
    if (total >= 12 && buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' &&
        buf[3] == 'F' && buf[8] == 'W' && buf[9] == 'E' && buf[10] == 'B' &&
        buf[11] == 'P') {
        return true;  // WebP
    }
    return false;
}

BinaryDownload download_image(const std::string& url) {
    BinaryDownload out;
    std::lock_guard<std::mutex> lock(s_http_mutex);
    auto http = create_http_client(out.err);
    if (http == nullptr) {
        return out;
    }
    // 覆盖通用 API 超时：正文分片到达可能很慢
    http->SetTimeout(kImageDownloadTimeoutMs);
    if (!http->Open("GET", url)) {
        out.err = "open failed";
        return out;
    }
    if (http->GetStatusCode() != 200) {
        char ebuf[32];
        std::snprintf(ebuf, sizeof(ebuf), "HTTP %d", http->GetStatusCode());
        http->Close();
        out.err = ebuf;
        return out;
    }

    constexpr size_t kInitialCap = 64 * 1024;
    constexpr size_t kChunk = 4 * 1024;
    size_t cap = kInitialCap;
    size_t total = 0;
    CapsBuffer buf = alloc_caps(cap);
    if (!buf) {
        http->Close();
        out.err = I18n::T("内存不足");
        return out;
    }
    CapsBuffer chunk = alloc_caps(kChunk);
    if (!chunk) {
        http->Close();
        out.err = I18n::T("内存不足");
        return out;
    }

    bool read_error = false;
    while (true) {
        int n = http->Read(reinterpret_cast<char*>(chunk.get()), kChunk);
        if (n < 0) {
            read_error = true;
            break;
        }
        if (n == 0) {
            break;
        }
        if (total + static_cast<size_t>(n) > kImageMaxBytes) {
            ESP_LOGE(TAG, "download image exceeds max %uKB total would be %u",
                     (unsigned)(kImageMaxBytes / 1024),
                     (unsigned)((total + static_cast<size_t>(n)) / 1024));
            read_error = true;
            break;
        }
        if (total + static_cast<size_t>(n) > cap) {
            size_t new_cap = cap * 2;
            while (new_cap < total + static_cast<size_t>(n)) {
                new_cap *= 2;
            }
            CapsBuffer nb = alloc_caps(new_cap);
            if (!nb) {
                read_error = true;
                break;
            }
            std::memcpy(nb.get(), buf.get(), total);
            buf = std::move(nb);
            cap = new_cap;
        }
        std::memcpy(buf.get() + total, chunk.get(), n);
        total += static_cast<size_t>(n);
    }
    http->Close();

    if (read_error || total == 0 || !looks_like_image(buf.get(), total)) {
        if (read_error) {
            out.err = I18n::T("图片下载失败");
            ESP_LOGE(TAG, "download image read timeout/error total=%u",
                     (unsigned)total);
        } else if (total == 0) {
            out.err = I18n::T("图片下载失败");
            ESP_LOGE(TAG, "download image empty body");
        } else {
            out.err = I18n::T("非图片响应");
            ESP_LOGE(TAG, "download image not image magic total=%u",
                     (unsigned)total);
        }
        return out;
    }
    out.ok = true;
    out.data = std::move(buf);
    out.size = total;
    return out;
}

bool parse_asr_text(const std::string& body, std::string& text_out,
                    std::string& err_out) {
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        err_out = I18n::T("转写结果解析失败");
        return false;
    }
    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        err_out = (cJSON_IsString(msg) && msg->valuestring != nullptr)
                      ? msg->valuestring
                      : I18n::T("转写失败");
        cJSON_Delete(root);
        return false;
    }
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsString(data) && data->valuestring != nullptr) {
        text_out = data->valuestring;
    } else if (cJSON_IsObject(data)) {
        cJSON* text = cJSON_GetObjectItemCaseSensitive(data, "text");
        if (cJSON_IsString(text) && text->valuestring != nullptr) {
            text_out = text->valuestring;
        }
    }
    cJSON_Delete(root);
    if (text_out.empty()) {
        err_out = I18n::T("未识别到有效文字");
        return false;
    }
    return true;
}

struct CreateTaskResult {
    bool ok = false;
    std::string task_id;
    std::vector<std::string> image_urls;
    std::string err;
};

void collect_image_urls(cJSON* urls, std::vector<std::string>& out) {
    if (!cJSON_IsArray(urls)) {
        return;
    }
    const int n = cJSON_GetArraySize(urls);
    for (int i = 0; i < n; ++i) {
        cJSON* u = cJSON_GetArrayItem(urls, i);
        if (cJSON_IsString(u) && u->valuestring != nullptr &&
            u->valuestring[0] != '\0') {
            out.emplace_back(u->valuestring);
        }
    }
}

CreateTaskResult parse_create_task(const std::string& body) {
    CreateTaskResult out;
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        out.err = I18n::T("生图响应解析失败");
        return out;
    }
    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        out.err = (cJSON_IsString(msg) && msg->valuestring != nullptr)
                      ? msg->valuestring
                      : I18n::T("生图请求失败");
        cJSON_Delete(root);
        return out;
    }
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(data)) {
        collect_image_urls(
            cJSON_GetObjectItemCaseSensitive(data, "imageUrls"),
            out.image_urls);
        static const char* kTaskIdKeys[] = {"taskId", "task_id", "id"};
        for (const char* key : kTaskIdKeys) {
            cJSON* cand = cJSON_GetObjectItemCaseSensitive(data, key);
            if (cJSON_IsString(cand) && cand->valuestring != nullptr &&
                cand->valuestring[0] != '\0') {
                out.task_id = cand->valuestring;
                break;
            }
        }
    }
    if (out.image_urls.empty() && out.task_id.empty()) {
        out.err = I18n::T("缺少 taskId");
        cJSON_Delete(root);
        return out;
    }
    out.ok = true;
    cJSON_Delete(root);
    return out;
}

struct TaskPollResult {
    bool ok = false;
    bool finished = false;
    bool succeeded = false;
    std::vector<std::string> image_urls;
    std::string err;
};

TaskPollResult parse_task_status(const std::string& body) {
    TaskPollResult out;
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        out.err = I18n::T("任务状态解析失败");
        return out;
    }
    cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        out.err = (cJSON_IsString(msg) && msg->valuestring != nullptr)
                      ? msg->valuestring
                      : I18n::T("查询任务失败");
        cJSON_Delete(root);
        return out;
    }
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data)) {
        out.err = I18n::T("任务状态解析失败");
        cJSON_Delete(root);
        return out;
    }
    cJSON* finished = cJSON_GetObjectItemCaseSensitive(data, "finished");
    out.finished = cJSON_IsTrue(finished);
    cJSON* status = cJSON_GetObjectItemCaseSensitive(data, "taskStatus");
    const char* st =
        (cJSON_IsString(status) && status->valuestring != nullptr)
            ? status->valuestring
            : "";
    out.succeeded = (std::strcmp(st, "SUCCEEDED") == 0);
    if (std::strcmp(st, "FAILED") == 0) {
        cJSON* message = cJSON_GetObjectItemCaseSensitive(data, "message");
        out.err = (cJSON_IsString(message) && message->valuestring != nullptr)
                      ? message->valuestring
                      : I18n::T("生图失败");
        cJSON_Delete(root);
        return out;
    }
    cJSON* urls = cJSON_GetObjectItemCaseSensitive(data, "imageUrls");
    collect_image_urls(urls, out.image_urls);
    out.ok = true;
    cJSON_Delete(root);
    return out;
}

// Returns true if at least one image was shown. On false, err_out holds UI text.
bool apply_images_ui(uint32_t session,
                     std::vector<BinaryDownload>& images,
                     std::string& err_out) {
    err_out.clear();
    if (!session_alive(session)) {
        images.clear();
        err_out = I18n::T("页面已关闭");
        ESP_LOGW(TAG, "apply_images_ui: session dead");
        return false;
    }
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        images.clear();
        err_out = I18n::T("图片展示失败");
        ESP_LOGW(TAG, "apply_images_ui: lv lock failed");
        return false;
    }
    if (!session_alive(session) || !screen_alive() || s_gallery == nullptr) {
        esp_lv_adapter_unlock();
        images.clear();
        err_out = I18n::T("页面已关闭");
        ESP_LOGW(TAG, "apply_images_ui: screen/gallery gone");
        return false;
    }

    clear_gallery_locked();

    std::vector<std::unique_ptr<LvglAllocatedImage>> decoded;
    decoded.reserve(images.size());
    for (auto& img : images) {
        if (!img.ok || !img.data || img.size == 0) {
            continue;
        }
        uint8_t* raw = img.data.release();
        try {
            auto holder = std::make_unique<LvglAllocatedImage>(raw, img.size);
            decoded.push_back(std::move(holder));
        } catch (const std::exception& ex) {
            ESP_LOGW(TAG, "Network operation failed");
            heap_caps_free(raw);
        }
    }
    images.clear();

    if (decoded.empty()) {
        esp_lv_adapter_unlock();
        err_out = I18n::T("图片解码失败");
        ESP_LOGW(TAG, "apply_images_ui: all decode failed");
        return false;
    }

    if (s_hint_lbl != nullptr) {
        lv_obj_add_flag(s_hint_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_prompt_lbl != nullptr) {
        lv_obj_set_height(s_prompt_lbl, 22);
    }
    lv_obj_update_layout(s_gallery);

    if (decoded.size() == 1) {
        make_image_card(s_gallery, decoded[0].get());
        s_image_holders = std::move(decoded);
        esp_lv_adapter_unlock();
        ESP_LOGI(TAG, "applied %u image(s)",
                 static_cast<unsigned>(s_image_holders.size()));
        return true;
    }

    lv_obj_t* tv = lv_tabview_create(s_gallery);
    lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv, kTabBarH);
    lv_obj_set_style_bg_color(tv, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tv, 0, LV_PART_MAIN);

    lv_obj_t* bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(bar, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorTabActive),
                              static_cast<lv_part_t>(LV_PART_ITEMS) |
                                  static_cast<lv_state_t>(LV_STATE_CHECKED));
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText),
                                static_cast<lv_part_t>(LV_PART_ITEMS) |
                                    static_cast<lv_state_t>(LV_STATE_CHECKED));
    screen_swipe_back_ignore(bar, true);

    lv_obj_t* content = lv_tabview_get_content(tv);
    // 禁止内容区左右滑动切 Tab，只能点顶部 Tab 项切换。
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    screen_swipe_back_ignore(content, true);

    for (size_t i = 0; i < decoded.size(); ++i) {
        char tab_name[12];
        std::snprintf(tab_name, sizeof(tab_name), "%d",
                      static_cast<int>(i + 1));
        lv_obj_t* tab = lv_tabview_add_tab(tv, tab_name);
        screen_strip_obj_chrome(tab);
        lv_obj_set_style_bg_color(tab, lv_color_hex(kColorBg), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_all(tab, 8, LV_PART_MAIN);
        lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
        screen_swipe_back_ignore(tab, true);
        make_image_card(tab, decoded[i].get());
    }

    s_image_holders = std::move(decoded);
    esp_lv_adapter_unlock();
    ESP_LOGI(TAG, "applied %u image(s)",
             static_cast<unsigned>(s_image_holders.size()));
    return true;
}

// ---------------------------------------------------------------------------
// Worker: record → ASR → baidu picture generate → (poll) → download
// ---------------------------------------------------------------------------
void worker_task(void* /*arg*/) {
    const uint32_t session = s_session.load(std::memory_order_acquire);
    auto& app = Application::GetInstance();
    auto& as = app.GetAudioService();

    const bool cellular = is_cellular_network();
    const int max_record_sec =
        cellular ? kMaxRecordSecondsCellular : kMaxRecordSecondsWifi;
    const size_t max_record_bytes =
        cellular ? kMaxRecordBytesCellular : kMaxRecordBytesWifi;

    CapsBuffer buffer = alloc_caps(max_record_bytes);
    if (!buffer) {
        buffer = alloc_caps(max_record_bytes / 4);
    }
    if (!buffer) {
        ESP_LOGE(TAG, "no memory for record buffer");
        finish_worker(session, I18n::T("内存不足"));
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    size_t written = 0;
    bool truncated = false;
    std::vector<int16_t> frame;
    frame.reserve(kSamplesPerFrame * 2);
    const int64_t start_us = esp_timer_get_time();
    s_record_start_us.store(start_us);

    {
        WakeWordGuard wake_guard(as);
        while (!s_stop_requested.load(std::memory_order_acquire) &&
               session_alive(session)) {
            if (!as.ReadAudioData(frame, kSampleRate, kSamplesPerFrame)) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            AudioCodec* codec = Board::GetInstance().GetAudioCodec();
            if (codec != nullptr && codec->input_channels() == 2) {
                keep_left_channel_only(frame);
            }
            const size_t bytes = frame.size() * sizeof(int16_t);
            if (written + bytes > max_record_bytes) {
                truncated = true;
                break;
            }
            std::memcpy(buffer.get() + written, frame.data(), bytes);
            written += bytes;
            if ((esp_timer_get_time() - start_us) / 1000 >=
                max_record_sec * 1000) {
                truncated = true;
                break;
            }
        }
    }

    if (!session_alive(session)) {
        clear_own_worker_handle();
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    const int duration_ms =
        static_cast<int>((esp_timer_get_time() - start_us) / 1000);
    if (duration_ms < kMinRecordMs || written < 1024) {
        finish_worker(session, I18n::T("录音太短，再试一次"));
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    if (truncated) {
        ESP_LOGW(TAG, "record truncated at %dms cellular=%d max_sec=%d",
                 duration_ms, cellular ? 1 : 0, max_record_sec);
    }

    set_state_ui(State::Busy, session);
    post_button_text(truncated ? I18n::T("过长已截断，识别中…")
                            : I18n::T("识别中…"),
                     session);

    uint8_t wav_header[44];
    fill_wav_header(wav_header, static_cast<uint32_t>(written));
    std::string wav_body;
    wav_body.reserve(sizeof(wav_header) + written);
    wav_body.assign(reinterpret_cast<const char*>(wav_header), sizeof(wav_header));
    wav_body.append(reinterpret_cast<const char*>(buffer.get()), written);
    buffer.reset();

    ESP_LOGI(TAG,
             "asr upload prepare wav_len=%u duration_ms=%d cellular=%d "
             "truncated=%d",
             static_cast<unsigned>(wav_body.size()), duration_ms,
             cellular ? 1 : 0, truncated ? 1 : 0);

    HttpJsonResult asr = http_post_binary(api::Url(api::kXiaozhiAsrWav),
                                          std::move(wav_body),
                                          "application/octet-stream");
    if (!session_alive(session)) {
        abort_worker_session_dead("after_asr");
    }
    if (!asr.ok) {
        std::string msg = std::string(I18n::T("识别失败: ")) + asr.err;
        finish_worker(session, msg.c_str());
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    std::string prompt;
    std::string parse_err;
    if (!parse_asr_text(asr.body, prompt, parse_err)) {
        ESP_LOGW(TAG, "Network operation failed");
        finish_worker(session, parse_err.c_str());
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    ESP_LOGI(TAG, "asr ok prompt_len=%u", static_cast<unsigned>(prompt.size()));
    asr.body.clear();
    asr.body.shrink_to_fit();

    int n = 1;
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (session_alive(session) && screen_alive()) {
            n = selected_image_count();
            set_prompt_locked(prompt);
            clear_gallery_locked();
        }
        esp_lv_adapter_unlock();
    }

    s_gen_start_us.store(esp_timer_get_time(), std::memory_order_release);
    post_button_text(I18n::T("生成中… 已用时 0 秒"), session);

    cJSON* req = cJSON_CreateObject();
    if (req == nullptr) {
        finish_worker(session, I18n::T("内存不足"));
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    cJSON_AddStringToObject(req, "prompt", prompt.c_str());
    cJSON_AddStringToObject(req, "size", kText2ImageSize);
    cJSON_AddNumberToObject(req, "n", n);
    char* req_raw = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (req_raw == nullptr) {
        finish_worker(session, I18n::T("内存不足"));
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    std::string req_json(req_raw);
    cJSON_free(req_raw);

    ESP_LOGI(TAG, "text2image create begin n=%d json_len=%u", n,
             static_cast<unsigned>(req_json.size()));
    HttpJsonResult create =
        http_post_json(api::Url(api::kText2Image), std::move(req_json));
    if (!session_alive(session)) {
        abort_worker_session_dead("after_create");
    }
    if (!create.ok) {
        std::string msg = std::string(I18n::T("生图失败: ")) + create.err;
        finish_worker(session, msg.c_str());
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    CreateTaskResult created = parse_create_task(create.body);
    create.body.clear();
    create.body.shrink_to_fit();
    if (!created.ok) {
        ESP_LOGW(TAG, "Network operation failed");
        finish_worker(session, created.err.c_str());
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    std::vector<std::string> image_urls = std::move(created.image_urls);
    if (!image_urls.empty()) {
        ESP_LOGI(TAG, "text2image sync count=%u n=%d",
                 static_cast<unsigned>(image_urls.size()), n);
    } else {
        ESP_LOGI(TAG, "text2image task created n=%d", n);

        int http_fails = 0;
        bool got_images = false;
        for (int attempt = 0; attempt < kMaxPollAttempts; ++attempt) {
            if (!session_alive(session)) {
                abort_worker_session_dead("poll_before_delay");
            }
            vTaskDelay(pdMS_TO_TICKS(attempt == 0 ? kPollFirstDelayMs
                                                    : kPollIntervalMs));
            if (!session_alive(session)) {
                abort_worker_session_dead("poll_after_delay");
            }

            HttpJsonResult poll =
                http_get_json(api::Text2ImageTaskUrl(created.task_id));
            if (!poll.ok) {
                ++http_fails;
                ESP_LOGW(TAG, "Network operation failed");
                if (http_fails >= kMaxPollHttpFails) {
                    finish_worker(session, I18n::T("查询任务失败"));
                    vTaskDeleteWithCaps(nullptr);
                    return;
                }
                continue;
            }
            http_fails = 0;
            TaskPollResult st = parse_task_status(poll.body);
            if (!st.ok) {
                ESP_LOGW(TAG, "Network operation failed");
                finish_worker(session,
                              st.err.empty() ? I18n::T("查询任务失败")
                                             : st.err.c_str());
                vTaskDeleteWithCaps(nullptr);
                return;
            }
            if (!st.image_urls.empty()) {
                image_urls = std::move(st.image_urls);
                got_images = true;
                ESP_LOGI(TAG, "poll ready attempt=%d urls=%u", attempt,
                         static_cast<unsigned>(image_urls.size()));
                break;
            }
            if (st.finished && !st.succeeded) {
                finish_worker(session, st.err.empty() ? I18n::T("生图失败")
                                                      : st.err.c_str());
                vTaskDeleteWithCaps(nullptr);
                return;
            }
            ESP_LOGI(TAG,
                     "poll pending attempt=%d/%d finished=%d succeeded=%d",
                     attempt + 1, kMaxPollAttempts, st.finished ? 1 : 0,
                     st.succeeded ? 1 : 0);
        }

        if (!got_images || image_urls.empty()) {
            finish_worker(session, I18n::T("生成超时，请重试"));
            vTaskDeleteWithCaps(nullptr);
            return;
        }
    }

    // 下载阶段：停止「生成中已用时」刷新，按钮改为下载中。
    s_gen_start_us.store(0, std::memory_order_release);
    post_button_text(I18n::T("下载中…"), session);

    std::vector<BinaryDownload> images;
    images.reserve(image_urls.size());
    int ok_count = 0;
    for (size_t i = 0; i < image_urls.size(); ++i) {
        if (!session_alive(session)) {
            abort_worker_session_dead("download");
        }
        BinaryDownload dl = download_image(image_urls[i]);
        if (dl.ok) {
            ++ok_count;
        } else {
            ESP_LOGW(TAG, "Network operation failed");
        }
        images.push_back(std::move(dl));
    }

    if (ok_count == 0) {
        finish_worker(session, I18n::T("图片下载失败"));
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    std::string apply_err;
    if (!apply_images_ui(session, images, apply_err)) {
        finish_worker(session,
                      apply_err.empty() ? I18n::T("图片展示失败")
                                        : apply_err.c_str());
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    finish_worker(session);
    vTaskDeleteWithCaps(nullptr);
}

// ---------------------------------------------------------------------------
// Events / UI build
// ---------------------------------------------------------------------------
void on_record_pressed(lv_event_t* /*e*/) {
    if (s_state.load(std::memory_order_acquire) != State::Idle) {
        return;
    }
    auto& app = Application::GetInstance();
    const DeviceState ds = app.GetDeviceState();
    if (ds == kDeviceStateConnecting || ds == kDeviceStateListening ||
        ds == kDeviceStateSpeaking || ds == kDeviceStateUpgrading) {
        if (s_record_lbl != nullptr) {
            lv_label_set_text(s_record_lbl, I18n::T("请先结束当前对话"));
        }
        return;
    }
    if (s_worker_task != nullptr) {
        return;
    }

    s_stop_requested.store(false, std::memory_order_release);
    s_state.store(State::Recording, std::memory_order_release);
    update_button_ui_locked(State::Recording);

    // 12KB 栈若从内部 DRAM 分配，碎片化时常失败（largest free < 12KB）。
    constexpr UBaseType_t kWorkerStackCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    BaseType_t ok = xTaskCreateWithCaps(worker_task, "ai_img_gen", 12288, nullptr,
                                        5, &s_worker_task, kWorkerStackCaps);
    if (ok != pdPASS) {
        ESP_LOGE(TAG,
                 "create worker failed (internal=%uKB largest=%uKB psram=%uKB)",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) /
                            1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        s_worker_task = nullptr;
        s_state.store(State::Idle, std::memory_order_release);
        update_button_ui_locked(State::Idle);
        if (s_record_lbl != nullptr) {
            lv_label_set_text(s_record_lbl, I18n::T("任务启动失败"));
        }
    }
}

void on_record_released(lv_event_t* /*e*/) {
    if (s_state.load() == State::Recording) {
        s_stop_requested.store(true);
    }
}

void on_swipe_back() {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void on_screen_unloaded(lv_event_t* e) {
    lv_obj_t* scr = lv_event_get_target_obj(e);
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_state.store(State::Closing, std::memory_order_release);
    s_stop_requested.store(true, std::memory_order_release);

    // 若本屏已被更新的 Create() 替换，静态指针属于新实例，不能在这里清掉。
    if (scr != s_screen) {
        ESP_LOGI(TAG, "unload: stale screen instance ignored");
        return;
    }

    if (s_tick_timer != nullptr) {
        lv_timer_delete(s_tick_timer);
        s_tick_timer = nullptr;
    }
    clear_gallery_locked();
    s_screen = nullptr;
    s_prompt_lbl = nullptr;
    s_hint_lbl = nullptr;
    s_gallery = nullptr;
    s_record_btn = nullptr;
    s_record_lbl = nullptr;
    s_n_dd = nullptr;
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
    lv_obj_set_style_text_font(list, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_radius(list, 10, LV_PART_MAIN);
    lv_obj_set_style_max_height(list, 160, LV_PART_MAIN);
    screen_swipe_back_ignore(list, true);
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
    lv_label_set_text(title, I18n::T("AI 绘图"));
    lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    screen_make_input_passive(title);

    // 数量下拉：Montserrat 才能渲染 LV_SYMBOL_DOWN；puhui 会显示成「1�」
    s_n_dd = lv_dropdown_create(top);
    lv_obj_set_size(s_n_dd, 52, kHeaderH - 4);
    lv_obj_align(s_n_dd, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_ext_click_area(s_n_dd, panel::kTouchExt);
    lv_obj_set_style_radius(s_n_dd, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_n_dd, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_n_dd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_n_dd, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_n_dd, lv_color_hex(0x4B5563),
                                  LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_n_dd, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_n_dd, 6, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_n_dd, lv_color_hex(kColorText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_n_dd, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_n_dd, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_dropdown_set_symbol(s_n_dd, LV_SYMBOL_DOWN);
    lv_dropdown_set_options(s_n_dd, "1\n2\n3\n4");
    lv_dropdown_set_selected(s_n_dd, 0);
    lv_obj_add_event_cb(s_n_dd, on_dropdown_ready, LV_EVENT_READY, nullptr);
    screen_swipe_back_ignore(s_n_dd, true);
}

void build_body(lv_obj_t* parent) {
    lv_obj_t* body = lv_obj_create(parent);
    screen_strip_obj_chrome(body);
    lv_obj_set_size(body, kPanelW, kBodyH);
    lv_obj_set_pos(body, 0, kHeaderH);
    lv_obj_set_style_bg_color(body, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(body, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_right(body, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_top(body, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(body, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 6, LV_PART_MAIN);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    s_prompt_lbl = lv_label_create(body);
    lv_obj_set_width(s_prompt_lbl, LV_PCT(100));
    lv_obj_set_height(s_prompt_lbl, kPromptAreaH);
    lv_label_set_long_mode(s_prompt_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_prompt_lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_prompt_lbl, lv_color_hex(kColorPromptText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_align(s_prompt_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    set_prompt_locked("");
    screen_make_input_passive(s_prompt_lbl);

    // 空态提示（对齐 HTML：描述画面，生成图片）
    s_hint_lbl = lv_label_create(body);
    lv_label_set_text(s_hint_lbl, I18n::T("描述画面，生成图片"));
    lv_obj_set_style_text_color(s_hint_lbl, lv_color_hex(kColorHintText),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_hint_lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_hint_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_hint_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_hint_lbl, LV_LABEL_LONG_WRAP);
    screen_make_input_passive(s_hint_lbl);

    s_gallery = lv_obj_create(body);
    screen_strip_obj_chrome(s_gallery);
    lv_obj_set_width(s_gallery, LV_PCT(100));
    lv_obj_set_flex_grow(s_gallery, 1);
    lv_obj_set_style_bg_opa(s_gallery, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(s_gallery, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(s_gallery, true);
}

void build_footer(lv_obj_t* parent) {
    lv_obj_t* footer = lv_obj_create(parent);
    screen_strip_obj_chrome(footer);
    lv_obj_set_size(footer, kPanelW, kFooterH);
    lv_obj_set_pos(footer, 0, kPanelH - kFooterH);
    lv_obj_set_style_bg_color(footer, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    constexpr int32_t kBtnW = kPanelW - 24;
    constexpr int32_t kBtnH = 36;
    s_record_btn = lv_button_create(footer);
    lv_obj_set_size(s_record_btn, kBtnW, kBtnH);
    lv_obj_align(s_record_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_record_btn, kBtnH / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_record_btn, lv_color_hex(kColorRecordBtnIdle),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_record_btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_record_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_record_btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_record_btn,
                              lv_color_hex(kColorRecordBtnActive),
                              LV_PART_MAIN | LV_STATE_PRESSED);

    s_record_lbl = lv_label_create(s_record_btn);
    lv_label_set_text(s_record_lbl, I18n::T("按住说话"));
    lv_obj_set_width(s_record_lbl, kBtnW - 24);
    // 短文案居中；超宽时 LVGL 自动改左对齐并启动循环滚动
    lv_obj_set_style_text_align(s_record_lbl, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_record_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    // 过长文案循环横滚；约 16px/s，与天气屏一致
    lv_obj_set_style_anim_duration(
        s_record_lbl, lv_anim_speed_clamped(16, 3000, 60000), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_record_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_record_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_center(s_record_lbl);

    lv_obj_add_event_cb(s_record_btn, on_record_pressed, LV_EVENT_PRESSED,
                        nullptr);
    lv_obj_add_event_cb(s_record_btn, on_record_released, LV_EVENT_RELEASED,
                        nullptr);
    screen_swipe_back_ignore(s_record_btn, true);
}

}  // namespace

lv_obj_t* AiImageGenScreen::Create() {
    // 递增 session，让可能仍在跑的旧 worker 自行退出；不要强行清空
    // s_worker_task，否则可能与仍存活的旧任务交错启动第二个 worker。
    s_session.fetch_add(1, std::memory_order_acq_rel);
    s_state.store(State::Idle, std::memory_order_release);
    s_stop_requested.store(false, std::memory_order_release);

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

    s_tick_timer = lv_timer_create(tick_timer_cb, 100, nullptr);
    lv_obj_add_event_cb(scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    screen_attach_swipe_back(scr, on_swipe_back);
    return scr;
}

void AiImageGenScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: ai_image_gen_screen");
        return;
    }
    ESP_LOGI(TAG, "unload: ai_image_gen_screen");
    s_state.store(State::Closing, std::memory_order_release);
    s_stop_requested.store(true, std::memory_order_release);
    // 生图页不持有语音 UI 会话；只确保不误开唤醒词。
    Application::GetInstance().GetAudioService().EnableWakeWordDetection(false);
}
