#include "screen_nav_keys.h"

#include <cstring>

#include "application.h"
#include "chat_screen/chat_screen.h"
#include "device_state.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "home_screen/home_screen.h"
#include "lvgl.h"
#include "network_screen/network_screen.h"
#include "pwr_key_handler.h"
#include "screen_util.h"
#include "settings_screen/settings_screen.h"
#include "standby_screen/standby_screen.h"
#include "weather_screen/weather_screen.h"

namespace {

constexpr const char* TAG = "ScreenNavKeys";

bool s_home_ptt_active = false;
// 按住 want=true，松手 want=false；每次按/松都投递一次 Apply，读取当时的 want。
bool s_ptt_want = false;

struct NavJob {
    ScreenNavKey key;
    bool double_click;
};

void ApplyPttWant(void* /*user_data*/) {
    auto& app = Application::GetInstance();
    if (s_ptt_want) {
        app.StartListening();
    } else {
        const auto st = app.GetDeviceState();
        if (st == kDeviceStateListening) {
            app.StopListening();
        } else if (st == kDeviceStateSpeaking) {
            app.ToggleChatState();
        }
    }
}

void ScheduleApplyPtt() {
    lv_async_call(ApplyPttWant, nullptr);
}

void ChatHomeClickJob(void* /*user_data*/) {
    // 聊天页单击 ○：说话中打断回待命；聆听中结束会话；待命无动作
    if (!ChatScreen::IsActive()) {
        return;
    }
    auto& app = Application::GetInstance();
    const auto st = app.GetDeviceState();
    if (st == kDeviceStateSpeaking || st == kDeviceStateListening) {
        ESP_LOGI(TAG, "home click -> toggle/abort (state=%d)", static_cast<int>(st));
        app.ToggleChatState();
    }
}

void NavJobRun(void* user_data) {
    auto* job = static_cast<NavJob*>(user_data);
    if (job == nullptr) {
        return;
    }
    const ScreenNavKey key = job->key;
    const bool dbl = job->double_click;
    delete job;

    if (NetworkScreen::IsNavigationBlocked()) {
        ESP_LOGI(TAG, "navigation ignored while network page is processing");
        return;
    }

    if (key == ScreenNavKey::Home && dbl) {
        ESP_LOGI(TAG, "home double -> chat page");
        HomeScreen::TryClosePowerDialog();
        HomeScreen::GoHomeToChat();
        return;
    }

    // 电源弹窗打开时：任意底栏键先收起（翻页 / 回首页 / 返回均覆盖）
    HomeScreen::TryClosePowerDialog();

    if (HomeScreen::IsActive()) {
        if (key == ScreenNavKey::Prev) {
            HomeScreen::FlipPage(-1);
        } else if (key == ScreenNavKey::Next) {
            HomeScreen::FlipPage(1);
        }
        return;
    }

    if (key == ScreenNavKey::Prev) {
        // 待机页靠点击/电源键回首页；忽略底栏 Prev，避免插电幽灵触点立刻打回。
        if (std::strcmp(PwrKey_ActiveScreen(), "standby") == 0) {
            ESP_LOGI(TAG, "prev ignored on standby");
            return;
        }
        ESP_LOGI(TAG, "prev -> back");
        // OTA 升级页：直接回主页 App 界面，不回设置上一级
        if (SettingsScreen::IsOtaDetailActive()) {
            SettingsScreen::ExitToHome();
            return;
        }
        if (NetworkScreen::TryHandleNavBack()) {
            return;
        }
        if (WeatherScreen::TryHandleNavBack()) {
            return;
        }
        if (screen_nav_try_return_to_settings()) {
            return;
        }
        lv_obj_t* old = lv_screen_active();
        lv_obj_t* home = HomeScreen::Create();
        lv_screen_load(home);
        if (old != nullptr && old != home) {
            lv_obj_delete_async(old);
        }
        return;
    }
}

}  // namespace

bool screen_nav_on_standby_press() {
    if (!StandbyScreen::IsActive()) {
        return false;
    }
    lv_async_call([](void*) { StandbyScreen::WakeFromTouch(); }, nullptr);
    return true;
}

bool screen_nav_on_home_press() {
    if (!ChatScreen::IsActive()) {
        return false;
    }
    if (s_home_ptt_active) {
        return true;
    }
    s_home_ptt_active = true;
    s_ptt_want = true;
    ESP_LOGI(TAG, "home hold -> PTT start");
    ScheduleApplyPtt();
    return true;
}

void screen_nav_on_home_release(uint32_t held_ms) {
    if (s_home_ptt_active) {
        s_home_ptt_active = false;
        s_ptt_want = false;
        ESP_LOGI(TAG, "home release -> PTT stop (held=%u)", held_ms);
        ScheduleApplyPtt();
        return;
    }
}

void screen_nav_on_chat_home_click() {
    lv_async_call(ChatHomeClickJob, nullptr);
}

void screen_nav_on_hardware_key(ScreenNavKey key, bool double_click) {
    lv_async_call(NavJobRun, new NavJob{key, double_click});
}

bool screen_nav_ptt_held() {
    return s_ptt_want;
}
