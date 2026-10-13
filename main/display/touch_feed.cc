#include "touch_feed.h"

#include "config.h"
#include "pwr_key_handler.h"
#include "screen_nav_keys.h"

#include <esp_log.h>
#include <cstdlib>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "lvgl.h"

namespace {

constexpr const char* kTag = "TouchFeed";
constexpr uint32_t kDefaultPeriodMs = 40;
constexpr uint32_t kMaxBackoffMs = 400;

esp_lcd_touch_handle_t s_handle = nullptr;
SemaphoreHandle_t s_mutex = nullptr;
TaskHandle_t s_task = nullptr;
volatile bool s_run = false;
uint32_t s_period_ms = kDefaultPeriodMs;

struct TouchSnapshot {
    bool pressed = false;
    int16_t x = 0;
    int16_t y = 0;
};

TouchSnapshot s_snap;

#if NAV_KEY_ENABLE
// 底栏键状态机（单击/双击 / 聊天页 ○ 按住说话）
bool s_nav_down = false;
int s_nav_key = -1;  // 0=prev 1=home 2=next
uint32_t s_nav_down_tick = 0;
uint32_t s_home_last_up_tick = 0;
bool s_home_pending_single = false;
bool s_home_ptt_armed = false;
bool s_nav_wake_only = false;
// 仅 USB 供电时：派发后短节流，挡住插电幽灵连发，不拖慢第一次按下。
uint32_t s_nav_usb_throttle_since = 0;  // 0=未节流

constexpr uint32_t kNavUsbThrottleMs = 350;

// mirror_y=1：屏外底栏 → 大幅负 y（实测约 -360）；屏内顶部返回 → y 大（≈200+）。
// 只吃 y∈[NAV_KEY_Y_MIN, NAV_KEY_Y_MAX]，不扩大触摸 y_max。
bool IsNavKeyYBand(int16_t y) {
    return y >= NAV_KEY_Y_MIN && y <= NAV_KEY_Y_MAX;
}

bool NavUsbThrottleActive() {
    if (s_nav_usb_throttle_since == 0) {
        return false;
    }
    if (lv_tick_elaps(s_nav_usb_throttle_since) >= kNavUsbThrottleMs) {
        s_nav_usb_throttle_since = 0;
        return false;
    }
    return true;
}

bool IsUsbNavQuietLog() {
    return PwrKey_IsUsbPowered() && NavUsbThrottleActive();
}

void ArmUsbNavThrottle() {
    if (!PwrKey_IsUsbPowered()) {
        return;
    }
    s_nav_usb_throttle_since = lv_tick_get();
}

int ClassifyNavKey(int16_t x) {
    // 三键很近（180/200/220）：取最近锚点，避免容差空隙漏判
    const int d_l = std::abs(x - NAV_KEY_LEFT_X);
    const int d_h = std::abs(x - NAV_KEY_HOME_X);
    const int d_r = std::abs(x - NAV_KEY_RIGHT_X);
    int best = 0;
    int best_d = d_l;
    if (d_h < best_d) {
        best = 1;
        best_d = d_h;
    }
    if (d_r < best_d) {
        best = 2;
        best_d = d_r;
    }
    if (best_d > NAV_KEY_X_SLOP) {
        return -1;
    }
    return best;
}

void HandleNavRelease(int key) {
    // USB 幽灵约 200ms 连发：仅对左右键节流，避免误伤 ○ 双击。
    if ((key == 0 || key == 2) && PwrKey_IsUsbPowered() &&
        NavUsbThrottleActive()) {
        return;
    }

    const uint32_t now = lv_tick_get();
    if (key == 1) {
        if (s_home_pending_single &&
            (now - s_home_last_up_tick) <= NAV_KEY_DBL_MS) {
            s_home_pending_single = false;
            screen_nav_on_hardware_key(ScreenNavKey::Home, true);
            s_home_last_up_tick = 0;
            return;
        }
        s_home_pending_single = true;
        s_home_last_up_tick = now;
        return;
    }
    s_home_pending_single = false;
    if (key == 0) {
        screen_nav_on_hardware_key(ScreenNavKey::Prev, false);
        ArmUsbNavThrottle();
    } else if (key == 2) {
        screen_nav_on_hardware_key(ScreenNavKey::Next, false);
        ArmUsbNavThrottle();
    }
}

void PollHomeDoubleTimeout() {
    if (!s_home_pending_single) {
        return;
    }
    if (lv_tick_elaps(s_home_last_up_tick) > NAV_KEY_DBL_MS) {
        s_home_pending_single = false;
        // 聊天页单击 ○：打断说话回待命 / 结束聆听
        screen_nav_on_chat_home_click();
    }
}

void PollHomePttHold() {
    if (!s_nav_down || s_nav_key != 1 || s_home_ptt_armed || s_nav_wake_only) {
        return;
    }
    // 按住超过 120ms 再尝试 PTT（仅聊天页生效）
    if (lv_tick_elaps(s_nav_down_tick) < 120) {
        return;
    }
    if (screen_nav_on_home_press()) {
        s_home_ptt_armed = true;
        s_home_pending_single = false;
    }
}
#endif  // NAV_KEY_ENABLE

#if TOUCH_FEED_DEBUG
bool s_log_was_pressed = false;
int s_log_last_x = -1;
int s_log_last_y = -1;

void LogSnapshotIfChanged(const TouchSnapshot& next) {
    if (!next.pressed) {
        if (s_log_was_pressed) {
            ESP_LOGW(kTag, "chip: released");
            s_log_was_pressed = false;
            s_log_last_x = -1;
            s_log_last_y = -1;
        }
        return;
    }

    const int dx = (s_log_last_x >= 0) ? (next.x - s_log_last_x) : 0;
    const int dy = (s_log_last_y >= 0) ? (next.y - s_log_last_y) : 0;
    const bool moved = !s_log_was_pressed || dx != 0 || dy != 0;
    if (moved) {
        ESP_LOGW(kTag, "chip: p0=(%d,%d) d=(%+d,%+d)%s", next.x, next.y, dx,
                 dy, s_log_was_pressed ? "" : " [down]");
    }

    s_log_was_pressed = true;
    s_log_last_x = next.x;
    s_log_last_y = next.y;
}
#endif

// 返回 true 表示本轮 I2C 读成功。
bool UpdateSnapshotFromChip() {
    if (s_handle == nullptr) {
        return false;
    }

    if (esp_lcd_touch_read_data(s_handle) != ESP_OK) {
        return false;
    }

    esp_lcd_touch_point_data_t points[1] = {};
    uint8_t cnt = 0;
    if (esp_lcd_touch_get_data(s_handle, points, &cnt, 1) != ESP_OK) {
        return false;
    }

    TouchSnapshot next = s_snap;
    if (cnt > 0) {
        next.pressed = true;
        next.x = static_cast<int16_t>(points[0].x);
        next.y = static_cast<int16_t>(points[0].y);
    } else {
        next.pressed = false;
    }

    if (s_mutex != nullptr &&
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_snap = next;
        xSemaphoreGive(s_mutex);
    }

#if TOUCH_FEED_DEBUG
    LogSnapshotIfChanged(next);
#endif
#if NAV_KEY_LOG_COORDS
    static bool s_was = false;
    if (next.pressed && !s_was) {
#if NAV_KEY_ENABLE
        // USB 节流窗口内降噪，避免插电幽灵刷屏。
        if (!IsUsbNavQuietLog())
#endif
        {
            ESP_LOGW(kTag, "touch down @(%d,%d)", next.x, next.y);
        }
    }
    s_was = next.pressed;
#endif
    return true;
}

void ReaderTask(void* /*arg*/) {
    const uint32_t period_ms = s_period_ms;
    uint32_t consecutive_err = 0;
#if TOUCH_FEED_DEBUG
    ESP_LOGI(kTag, "reader started, period=%u ms", period_ms);
#endif

    while (s_run) {
        if (UpdateSnapshotFromChip()) {
            consecutive_err = 0;
            vTaskDelay(pdMS_TO_TICKS(period_ms));
        } else {
            if (consecutive_err < 8) {
                consecutive_err++;
            }
            uint32_t backoff = period_ms << (consecutive_err > 3 ? 3 : consecutive_err);
            if (backoff > kMaxBackoffMs) {
                backoff = kMaxBackoffMs;
            }
            if (consecutive_err == 1 || consecutive_err == 4 || consecutive_err == 8) {
                ESP_LOGW(kTag, "I2C read fail, backoff %u ms (n=%u)", backoff,
                         consecutive_err);
            }
            vTaskDelay(pdMS_TO_TICKS(backoff));
        }
    }

    s_task = nullptr;
    vTaskDelete(nullptr);
}

void IndevReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;
    if (data == nullptr) {
        return;
    }

    TouchSnapshot snap;
    if (s_mutex != nullptr &&
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        snap = s_snap;
        xSemaphoreGive(s_mutex);
    }

#if NAV_KEY_ENABLE
    PollHomeDoubleTimeout();
    PollHomePttHold();

    // 屏外/贴底 Y 带：一律不喂给 LVGL（避免负 y 被夹成 (x,0) 误点顶部）
    if (snap.pressed && IsNavKeyYBand(snap.y)) {
        const int key = ClassifyNavKey(snap.x);
        if (key >= 0) {
            if (!s_nav_down) {
                s_nav_down = true;
                s_nav_key = key;
                s_nav_down_tick = lv_tick_get();
                s_home_ptt_armed = false;
                s_nav_wake_only = screen_nav_on_standby_press();
                if (s_nav_wake_only) {
                    s_home_pending_single = false;
                    s_home_last_up_tick = 0;
                }
                ESP_LOGI(kTag, "nav down key=%d @(%d,%d)", key, snap.x, snap.y);
            }
        }
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = 0;
        data->point.y = 0;
        return;
    }

    if (!snap.pressed && s_nav_down) {
        const int key = s_nav_key;
        const uint32_t held = lv_tick_elaps(s_nav_down_tick);
        const bool was_ptt = s_home_ptt_armed;
        const bool wake_only = s_nav_wake_only;
        s_nav_down = false;
        s_nav_key = -1;
        s_home_ptt_armed = false;
        s_nav_wake_only = false;
        if (wake_only) {
            // The entire wake gesture is consumed, including its release.
        } else if (was_ptt && key == 1) {
            screen_nav_on_home_release(held);
        } else if (held < 800 && key >= 0) {
            HandleNavRelease(key);
        }
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = 0;
        data->point.y = 0;
        return;
    }
#endif

    data->point.x = snap.x;
    data->point.y = snap.y;
    if (data->point.x < 0) {
        data->point.x = 0;
    } else if (data->point.x > DISPLAY_WIDTH - 1) {
        data->point.x = DISPLAY_WIDTH - 1;
    }
    if (data->point.y < 0) {
        data->point.y = 0;
    } else if (data->point.y > DISPLAY_HEIGHT - 1) {
        data->point.y = DISPLAY_HEIGHT - 1;
    }
    data->state =
        snap.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace

void touch_feed_init(esp_lcd_touch_handle_t handle, uint32_t period_ms) {
    touch_feed_stop();

    s_handle = handle;
    s_period_ms = (period_ms == 0) ? kDefaultPeriodMs : period_ms;

    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (s_mutex == nullptr) {
        ESP_LOGE(kTag, "mutex create failed");
        return;
    }

    {
        TouchSnapshot cleared;
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            s_snap = cleared;
            xSemaphoreGive(s_mutex);
        }
    }
#if TOUCH_FEED_DEBUG
    s_log_was_pressed = false;
    s_log_last_x = -1;
    s_log_last_y = -1;
#endif

    s_run = true;
    // 内部 DRAM 栈：任务内会走 I2C，且可能触发震动/日志；PSRAM 栈读 Flash 会 assert。
    if (xTaskCreate(ReaderTask, "touch_feed", 4096, nullptr, 5, &s_task) !=
        pdPASS) {
        s_run = false;
        s_task = nullptr;
        ESP_LOGE(kTag, "xTaskCreate(touch_feed) failed");
        return;
    }
}

void touch_feed_attach_indev(lv_indev_t* indev) {
    if (indev == nullptr) {
        ESP_LOGW(kTag, "attach_indev: null indev");
        return;
    }
    lv_indev_set_read_cb(indev, IndevReadCb);
    // CST816 坐标有抖动；默认 10px 就会把轻点判成拖动，CLICKED 发不出来。
    lv_indev_set_scroll_limit(indev, 24);
}

void touch_feed_stop() {
    if (s_task != nullptr) {
        s_run = false;
        for (int i = 0; i < 50 && s_task != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (s_task != nullptr) {
            vTaskDelete(s_task);
            s_task = nullptr;
        }
    }
    s_run = false;
    s_handle = nullptr;
}
