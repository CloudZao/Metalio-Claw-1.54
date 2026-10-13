#include "backlight_screen.h"
#include "i18n.h"

#include <cstdio>

#include <esp_log.h>

#include "backlight.h"
#include "board.h"
#include "home_screen/home_screen.h"
#include "idle_power_policy.h"
#include "panel_metrics.h"
#include "screen_util.h"
#include "settings.h"

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "BacklightScreen";

constexpr int kPanelSize = panel::kSize;
constexpr int kHeaderH = panel::kHeaderH;
constexpr int kPad = panel::kPad;

struct UiState {
    lv_obj_t* screen = nullptr;
    lv_obj_t* pct_label = nullptr;
    lv_obj_t* slider = nullptr;
};
UiState s_ui;

int ReadInitialBrightness() {
    int value = kBacklightDefaultPercent;
    if (Backlight* backlight = Board::GetInstance().GetBacklight()) {
        value = backlight->brightness();
    } else {
        Settings settings("display");
        value = settings.GetInt("brightness", kBacklightDefaultPercent);
    }
    if (value < static_cast<int>(kBacklightMinPercent)) {
        value = kBacklightMinPercent;
    }
    if (value > 100) {
        value = 100;
    }
    return value;
}

void UpdatePctLabel(int pct) {
    if (s_ui.pct_label == nullptr) {
        return;
    }
    if (pct < static_cast<int>(kBacklightMinPercent)) {
        pct = kBacklightMinPercent;
    } else if (pct > 100) {
        pct = 100;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(s_ui.pct_label, buf);
}

void ApplyBrightness(int value) {
    if (value < static_cast<int>(kBacklightMinPercent)) {
        value = kBacklightMinPercent;
    }
    Backlight* backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr) {
        backlight->SetBrightness(static_cast<uint8_t>(value), true);
    }
}

void OnSliderChanged(lv_event_t* e) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int value = static_cast<int>(lv_slider_get_value(slider));
    if (value < static_cast<int>(kBacklightMinPercent)) {
        value = kBacklightMinPercent;
        lv_slider_set_value(slider, value, LV_ANIM_OFF);
    }
    UpdatePctLabel(value);
    ApplyBrightness(value);
}

void OnSwipeBack() {
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    if (screen_nav_try_return_to_settings()) {
        return;
    }
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    s_ui.screen = nullptr;
    s_ui.pct_label = nullptr;
    s_ui.slider = nullptr;
}

lv_obj_t* CreateSlider(lv_obj_t* parent, int initial_brightness) {
    lv_obj_t* slider_wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(slider_wrap);
    lv_obj_set_width(slider_wrap, LV_PCT(100));
    lv_obj_set_height(slider_wrap, 36);
    lv_obj_set_style_bg_opa(slider_wrap, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(slider_wrap, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(slider_wrap, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* slider = lv_slider_create(slider_wrap);
    s_ui.slider = slider;
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_set_height(slider, 18);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
    lv_slider_set_range(slider, kBacklightMinPercent, 100);
    lv_slider_set_value(slider, initial_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3B82F6), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_pad_hor(slider, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(slider, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 8, LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider, OnSliderChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    screen_swipe_back_ignore(slider, true);
    return slider_wrap;
}

}  // namespace

lv_obj_t* BacklightScreen::Create() {
    const int initial_brightness = ReadInitialBrightness();

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1116), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* header = lv_obj_create(scr);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("背光"));
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    // HTML 风格：简洁 list 行 + 滑条
    lv_obj_t* list = lv_obj_create(scr);
    screen_strip_obj_chrome(list);
    lv_obj_set_size(list, kPanelSize - 2 * kPad, kPanelSize - kHeaderH - kPad);
    lv_obj_set_pos(list, kPad, kHeaderH);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    auto make_row = [&](const char* left, lv_obj_t** right_out) {
        lv_obj_t* row = lv_obj_create(list);
        screen_strip_obj_chrome(row);
        lv_obj_set_size(row, LV_PCT(100), 32);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1B2030), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 10, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* l = lv_label_create(row);
        lv_label_set_text(l, left);
        lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(l, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* r = lv_label_create(row);
        *right_out = r;
        lv_obj_set_style_text_color(r, lv_color_hex(0x9AA3B2), LV_PART_MAIN);
        lv_obj_set_style_text_font(r, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
        return row;
    };

    make_row(I18n::T("亮度"), &s_ui.pct_label);
    UpdatePctLabel(initial_brightness);

    CreateSlider(list, initial_brightness);

    lv_obj_t* sleep_right = nullptr;
    make_row(I18n::T("自动息屏"), &sleep_right);
    {
        char buf[24];
        const int mins = IdlePower_GetStandbyMinutes();
        if (mins <= 0) {
            std::snprintf(buf, sizeof(buf), "%s", I18n::T("关"));
        } else if (mins == 1) {
            std::snprintf(buf, sizeof(buf), "60 %s", I18n::T("秒"));
        } else {
            std::snprintf(buf, sizeof(buf), "%d %s", mins, I18n::T("分钟"));
        }
        lv_label_set_text(sleep_right, buf);
    }

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    return scr;
}

void BacklightScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: backlight_screen");
    } else {
        ESP_LOGI(TAG, "unload: backlight_screen");
    }
}
