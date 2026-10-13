#include "pwr_key_handler.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include "application.h"
#include "button.h"
#include "home_screen/home_screen.h"
#include "standby_screen/standby_screen.h"

namespace {

constexpr const char* TAG = "PwrKey";
// 栈空 / 未知前台：短按不进待机（绝不能回落成 "home"，否则二级页
// 父页 UNLOAD 时会被误判为首页）。
constexpr const char* kNoneScreen = "none";
constexpr const char* kHomeScreen = "home";
constexpr const char* kStandbyScreen = "standby";
constexpr uint32_t kLongPressMs = 1500;
// 短于默认 180ms：插电毛刺更不易被判成 SINGLE_CLICK。
constexpr uint16_t kShortPressMs = 350;
// 主屏 ↔ 待机 切换防抖：避免按键抖动 / 二次 SINGLE_CLICK 在 Show 完成后
// 立刻再触发 ReturnHome，造成「进待机又马上回主屏」。
constexpr int64_t kHomeStandbyToggleGuardUs = 700000;  // 700ms
// Type-C 插入瞬间 GPIO47 易毛刺：忽略短按一段时间。
constexpr int64_t kUsbPlugSuppressUs = 2000000;  // 2s
constexpr int kMaxStack = 8;

enum class HomeStandbyPending : uint8_t {
    None = 0,
    ToStandby,
    ToHome,
};

const char* s_stack[kMaxStack] = {};
int s_depth = 0;
bool s_inited = false;
HomeStandbyPending s_hs_pending = HomeStandbyPending::None;
int64_t s_hs_guard_until_us = 0;
bool s_usb_powered = false;
int64_t s_usb_suppress_until_us = 0;

bool IsChatToggleScreen(const char* name) {
    return std::strcmp(name, "chat") == 0;
}

void StackPush(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    if (s_depth < kMaxStack) {
        s_stack[s_depth++] = name;
    } else {
        s_stack[kMaxStack - 1] = name;
        ESP_LOGW(TAG, "screen stack full, replace top with %s", name);
    }
}

void StackRemoveTopmost(const char* name) {
    if (name == nullptr || s_depth <= 0) {
        return;
    }
    for (int i = s_depth - 1; i >= 0; --i) {
        if (std::strcmp(s_stack[i], name) == 0) {
            for (int j = i; j < s_depth - 1; ++j) {
                s_stack[j] = s_stack[j + 1];
            }
            --s_depth;
            s_stack[s_depth] = nullptr;
            return;
        }
    }
}

const char* StackTop() {
    return s_depth > 0 ? s_stack[s_depth - 1] : kNoneScreen;
}

void OnEnterStandbyAsync(void* /*arg*/) {
    StandbyScreen::Show();
    s_hs_pending = HomeStandbyPending::None;
}

void OnLeaveStandbyAsync(void* /*arg*/) {
    StandbyScreen::ReturnHome();
    s_hs_pending = HomeStandbyPending::None;
}

bool HomeStandbyToggleBusy() {
    if (s_hs_pending != HomeStandbyPending::None) {
        return true;
    }
    return esp_timer_get_time() < s_hs_guard_until_us;
}

void ArmHomeStandbyToggleGuard() {
    s_hs_guard_until_us = esp_timer_get_time() + kHomeStandbyToggleGuardUs;
}

bool UsbPlugSuppressActive() {
    return esp_timer_get_time() < s_usb_suppress_until_us;
}

void OnShortPress() {
    const char* screen = PwrKey_ActiveScreen();
    ESP_LOGI(TAG, "short-press on screen=%s (depth=%d)", screen, s_depth);

    const bool on_home = std::strcmp(screen, kHomeScreen) == 0;
    const bool on_standby = std::strcmp(screen, kStandbyScreen) == 0;
    if (on_home || on_standby) {
        // 切屏异步未完成 / 防抖窗口内：忽略后续短按，防止进待机后立刻回主屏。
        if (HomeStandbyToggleBusy()) {
            ESP_LOGW(TAG, "dispatch: ignore home<->standby toggle (busy/guard)");
            return;
        }
        // 插电边沿毛刺窗口：home↔standby 短按一律忽略。
        if (UsbPlugSuppressActive()) {
            ESP_LOGW(TAG, "dispatch: ignore home<->standby (usb plug suppress)");
            return;
        }
        if (on_home) {
            ESP_LOGI(TAG, "dispatch: enter standby_screen");
            s_hs_pending = HomeStandbyPending::ToStandby;
            ArmHomeStandbyToggleGuard();
            lv_async_call(OnEnterStandbyAsync, nullptr);
        } else {
            ESP_LOGI(TAG, "dispatch: leave standby -> home");
            s_hs_pending = HomeStandbyPending::ToHome;
            ArmHomeStandbyToggleGuard();
            lv_async_call(OnLeaveStandbyAsync, nullptr);
        }
        return;
    }

    if (IsChatToggleScreen(screen)) {
        // 与主屏一致：GPIO47 插电毛刺会持续误触发短按，充电中禁止 Toggle。
        if (UsbPlugSuppressActive()) {
            ESP_LOGW(TAG, "dispatch: ignore chat ToggleChatState (usb plug suppress)");
            return;
        }
        if (s_usb_powered) {
            ESP_LOGW(TAG, "dispatch: ignore chat ToggleChatState (usb powered)");
            return;
        }
        ESP_LOGI(TAG, "dispatch: ToggleChatState()");
        Application::GetInstance().ToggleChatState();
        return;
    }

    ESP_LOGI(TAG, "dispatch: no-op (screen has no short-press action)");
}

void OnLongPressAsync(void* /*arg*/) {
    HomeScreen::ShowPowerOptionsDialog();
}

void OnLongPress() {
    const char* screen = PwrKey_ActiveScreen();
    ESP_LOGI(TAG, "long-press %ums on screen=%s -> power dialog",
             static_cast<unsigned>(kLongPressMs), screen);
    lv_async_call(OnLongPressAsync, nullptr);
}

}  // namespace

void PwrKey_Init(gpio_num_t gpio_num, bool active_high) {
    if (s_inited) {
        return;
    }

    if (gpio_num != GPIO_NUM_NC) {
        static Button* s_btn = nullptr;
        s_btn = new Button(gpio_num, active_high, kLongPressMs, kShortPressMs);
        s_btn->OnClick(OnShortPress);
        s_btn->OnLongPress(OnLongPress);
        s_inited = true;
        s_depth = 0;
        s_hs_pending = HomeStandbyPending::None;
        s_hs_guard_until_us = 0;
        s_usb_powered = false;
        s_usb_suppress_until_us = 0;
        ESP_LOGI(TAG,
                 "armed GPIO %d: short=%ums long=%ums (active_screen=%s)",
                 static_cast<int>(gpio_num),
                 static_cast<unsigned>(kShortPressMs),
                 static_cast<unsigned>(kLongPressMs), PwrKey_ActiveScreen());
        return;
    }

    ESP_LOGW(TAG, "PwrKey_Init: no GPIO, skip");
}

void PwrKey_Init() {
    PwrKey_Init(GPIO_NUM_NC, false);
}

void PwrKey_OnUsbPowerChanged(bool usb_powered) {
    const bool was = s_usb_powered;
    s_usb_powered = usb_powered;
    if (usb_powered && !was) {
        s_usb_suppress_until_us = esp_timer_get_time() + kUsbPlugSuppressUs;
        ESP_LOGI(TAG, "usb plugged: suppress short-press %lld ms",
                 static_cast<long long>(kUsbPlugSuppressUs / 1000));
    } else if (!usb_powered && was) {
        s_usb_suppress_until_us = 0;
        ESP_LOGI(TAG, "usb unplugged");
    }
}

bool PwrKey_IsUsbPowered() {
    return s_usb_powered;
}

void PwrKey_OnScreenLifecycle(const char* name,
                              screen_lifecycle_event_t event) {
    if (name == nullptr || name[0] == '\0') {
        name = kNoneScreen;
    }

    if (event == SCREEN_LIFECYCLE_LOAD) {
        StackPush(name);
        ESP_LOGD(TAG, "active_screen -> %s (load %s, depth=%d)", StackTop(),
                 name, s_depth);
        return;
    }

    // UNLOAD：只移除栈里最靠上的同名页，绝不能回落成 "home"。
    // 否则 Test→AutoTest 这类「父页销毁、子页未登记」会误进待机。
    StackRemoveTopmost(name);
    ESP_LOGD(TAG, "active_screen -> %s (unload %s, depth=%d)", StackTop(), name,
             s_depth);
}

const char* PwrKey_ActiveScreen() {
    return StackTop();
}
