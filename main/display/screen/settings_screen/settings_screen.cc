#include "settings_screen.h"

#include <cstdint>
#include <cstdio>

#include <esp_app_desc.h>
#include <esp_log.h>

#include "audio_codec.h"
#include "backlight.h"
#include "backlight_screen/backlight_screen.h"
#include "board.h"
#include "dual_network_board.h"
#include "home_screen/home_screen.h"
#include "i18n.h"
#include "info_screen/info_screen.h"
#include "network_screen/network_screen.h"
#include "panel_metrics.h"
#include "sdkconfig.h"
#include "screen_util.h"
#include "settings.h"
#include "theme_manager.h"
#include "theme_screen/theme_screen.h"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "SettingsScreen";

constexpr int kPanelSize = panel::kSize;
constexpr int kHeaderH = panel::kHeaderH;
constexpr int kTabBarW = 56;
constexpr int kTabItemH = 36;
constexpr int kTabItemGap = 4;
constexpr int kBodyH = kPanelSize - kHeaderH;
constexpr int kRowH = 36;
constexpr int kRowGap = 4;
constexpr int kListPad = 8;
constexpr int kRowIcon = 24;  // 与 ic_s_row_*.png 原生尺寸一致，1:1 绘制

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorSubtle = 0x9AA3B2;
constexpr uint32_t kColorCard = 0x1B2030;
constexpr uint32_t kColorTabBar = 0x12151C;
constexpr uint32_t kColorAccent = 0x3B82F6;
constexpr uint32_t kColorValue = 0x60A5FA;
constexpr uint32_t kColorSliderTrack = 0x2A2F3A;

enum class HubAction : int {
    Network = 1,
    Theme,
    Backlight,
    Volume,
    Ota,
    Info,
};

enum class ViewMode {
    Hub,
    DetailVolume,
    DetailOta,
};

struct UiState {
    lv_obj_t* screen = nullptr;
    lv_obj_t* title_lbl = nullptr;
    lv_obj_t* body = nullptr;
    lv_obj_t* tabview = nullptr;
    lv_obj_t* brightness_pct_label = nullptr;
    lv_obj_t* brightness_slider = nullptr;
    lv_obj_t* volume_pct_label = nullptr;
    lv_obj_t* volume_slider = nullptr;
    ViewMode mode = ViewMode::Hub;
};
UiState s_ui;


void OnSwipeBack();

int ReadInitialVolume() {
    int volume = 70;
    if (AudioCodec* codec = Board::GetInstance().GetAudioCodec()) {
        volume = codec->output_volume();
    }
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    return volume;
}

void UpdatePctLabel(lv_obj_t* label, int pct) {
    if (label == nullptr) {
        return;
    }
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(label, buf);
}

void ApplyVolume(int volume) {
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }

    UpdatePctLabel(s_ui.volume_pct_label, volume);

    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr || codec->output_volume() == volume) {
        return;
    }
    // 设置页调音时确保输出通路打开，否则 ES8311 设备句柄为空会写失败
    codec->EnableOutput(true);
    codec->SetOutputVolume(volume);
}

void OnVolumeSliderChanged(lv_event_t* e) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int value = static_cast<int>(lv_slider_get_value(slider));
    ApplyVolume(value);
    if (value != static_cast<int>(lv_slider_get_value(slider))) {
        lv_slider_set_value(slider, value, LV_ANIM_OFF);
    }
}

void StyleSlider(lv_obj_t* slider) {
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_set_height(slider, 16);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorSliderTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(kColorAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_pad_hor(slider, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(slider, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slider, 3, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 8, LV_PART_INDICATOR);
    screen_swipe_back_ignore(slider, true);
}

lv_obj_t* CreateSliderRow(lv_obj_t* parent, int min_value, int max_value,
                          int initial_value, lv_event_cb_t cb,
                          lv_obj_t** out_slider) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* slider = lv_slider_create(row);
    if (out_slider != nullptr) {
        *out_slider = slider;
    }
    StyleSlider(slider);
    lv_slider_set_range(slider, min_value, max_value);
    lv_slider_set_value(slider, initial_value, LV_ANIM_OFF);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return row;
}

void BuildSliderPanel(lv_obj_t* parent, const char* title, const char* hint,
                      const char* range_hint, int initial_value,
                      lv_obj_t** pct_label_out, lv_obj_t** slider_out,
                      int slider_min, int slider_max, lv_event_cb_t slider_cb,
                      int card_height = 72) {
    lv_obj_set_style_pad_all(parent, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(parent, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(parent, LV_DIR_VER);

    lv_obj_t* card = lv_obj_create(parent);
    screen_strip_obj_chrome(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, card_height);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    screen_make_input_passive(card);

    lv_obj_t* pct = lv_label_create(card);
    if (pct_label_out != nullptr) {
        *pct_label_out = pct;
    }
    lv_obj_set_width(pct, LV_PCT(100));
    lv_label_set_long_mode(pct, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(pct, lv_color_hex(kColorValue), LV_PART_MAIN);
    lv_obj_set_style_text_font(pct, &font_puhui_30_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(pct, LV_ALIGN_CENTER, 0, -6);
    UpdatePctLabel(pct, initial_value);

    lv_obj_t* card_hint = lv_label_create(card);
    lv_label_set_text(card_hint, hint);
    lv_obj_set_style_text_color(card_hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(card_hint, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(card_hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_obj_t* slider_hdr = lv_obj_create(parent);
    lv_obj_remove_style_all(slider_hdr);
    lv_obj_set_width(slider_hdr, LV_PCT(100));
    lv_obj_set_height(slider_hdr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(slider_hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(slider_hdr, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(slider_hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* slider_title = lv_label_create(slider_hdr);
    lv_label_set_text(slider_title, title);
    lv_obj_set_style_text_color(slider_title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(slider_title, &font_puhui_20_4, LV_PART_MAIN);

    lv_obj_t* range_lbl = lv_label_create(slider_hdr);
    lv_label_set_text(range_lbl, range_hint);
    lv_obj_set_style_text_color(range_lbl, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(range_lbl, &font_puhui_20_4, LV_PART_MAIN);

    CreateSliderRow(parent, slider_min, slider_max, initial_value, slider_cb,
                    slider_out);
}

void BuildVolumeTab(lv_obj_t* tab, int initial_volume) {
    BuildSliderPanel(tab, I18n::T("拖动调节"), I18n::T("当前音量"), "0% ~ 100%",
                     initial_volume, &s_ui.volume_pct_label, &s_ui.volume_slider,
                     0, 100, OnVolumeSliderChanged);

    lv_obj_t* foot = lv_label_create(tab);
    lv_label_set_text(foot, I18n::T("音量设置会自动保存"));
    lv_obj_set_style_text_color(foot, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(foot, &font_puhui_20_4, LV_PART_MAIN);
}

void BuildHeader(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(header);
    s_ui.title_lbl = title;
    lv_label_set_text(title, I18n::T("设置"));
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
}

void SetTitle(const char* text) {
    if (s_ui.title_lbl != nullptr) {
        lv_label_set_text(s_ui.title_lbl, text);
        lv_obj_align(s_ui.title_lbl, LV_ALIGN_CENTER, 0, 0);
    }
}

void ClearBody() {
    if (s_ui.body == nullptr) {
        return;
    }
    lv_obj_clean(s_ui.body);
    s_ui.tabview = nullptr;
    s_ui.brightness_pct_label = nullptr;
    s_ui.brightness_slider = nullptr;
    s_ui.volume_pct_label = nullptr;
    s_ui.volume_slider = nullptr;
}

void LaunchSubScreen(lv_obj_t* (*create_fn)(),
                     screen_lifecycle_cb_t lifecycle_cb) {
    screen_nav_set_return_to_settings(true);
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = create_fn();
    if (lifecycle_cb != nullptr) {
        screen_attach_lifecycle(app, lifecycle_cb);
    }
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void ShowHubList();
void ShowDetailVolume();
void ShowDetailOta();

void OnHubRowClicked(lv_event_t* e) {
    const auto action = static_cast<HubAction>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    switch (action) {
        case HubAction::Network:
            LaunchSubScreen(NetworkScreen::Create,
                            NetworkScreen::LifecycleCallback);
            break;
        case HubAction::Theme:
            LaunchSubScreen(ThemeScreen::Create, ThemeScreen::LifecycleCallback);
            break;
        case HubAction::Backlight:
            LaunchSubScreen(BacklightScreen::Create,
                            BacklightScreen::LifecycleCallback);
            break;
        case HubAction::Info:
            LaunchSubScreen(InfoScreen::Create, InfoScreen::LifecycleCallback);
            break;
        case HubAction::Volume:
            ShowDetailVolume();
            break;
        case HubAction::Ota:
            ShowDetailOta();
            break;
    }
}

void AddHubRow(lv_obj_t* list, const char* name, const char* icon_src,
               const char* right, HubAction action) {
    lv_obj_t* row = lv_obj_create(list);
    screen_strip_obj_chrome(row);
    lv_obj_set_size(row, LV_PCT(100), kRowH);
    lv_obj_set_style_bg_color(row, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 8, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, OnHubRowClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(action)));
    screen_swipe_back_ignore(row, true);

    int text_x = 0;
    if (icon_src != nullptr && icon_src[0] != '\0') {
        lv_obj_t* icon = lv_image_create(row);
        lv_image_set_src(icon, icon_src);
        // ic_s_row_* 为 24×24 SPNG，须 1:1，勿再缩放
        lv_obj_set_size(icon, kRowIcon, kRowIcon);
        lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        text_x = kRowIcon + 8;
    }

    lv_obj_t* left = lv_label_create(row);
    lv_label_set_text(left, name);
    lv_obj_set_style_text_color(left, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(left, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(left, LV_ALIGN_LEFT_MID, text_x, 0);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_CLICKABLE);

    if (right != nullptr && right[0] != '\0') {
        lv_obj_t* r = lv_label_create(row);
        lv_label_set_text(r, right);
        lv_obj_set_style_text_color(r, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(r, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    }
}

void ShowHubList() {
    ClearBody();
    s_ui.mode = ViewMode::Hub;
    SetTitle(I18n::T("设置"));

    lv_obj_t* list = lv_obj_create(s_ui.body);
    screen_strip_obj_chrome(list);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, kRowGap, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    screen_swipe_back_ignore(list, true);

    char theme_right[24];
    std::snprintf(theme_right, sizeof(theme_right), I18n::T("主题 %d"),
                  ThemeManager::GetCurrentThemeId());

    char bright_right[16] = "--";
    if (Backlight* bl = Board::GetInstance().GetBacklight()) {
        std::snprintf(bright_right, sizeof(bright_right), "%d%%",
                      bl->brightness());
    }

    char vol_right[16] = "--";
    std::snprintf(vol_right, sizeof(vol_right), "%d%%", ReadInitialVolume());

    // 网络 → 主题 → 音量 → 背光 → OTA → 系统信息
    const char* net_right = "Wi-Fi";
    {
        const NetworkType nt =
            DualNetworkBoard::LoadNetworkTypeFromSettings(1);
        if (nt == NetworkType::ML307) {
            net_right = "4G";
        }
    }
    AddHubRow(list, I18n::T("网络配置"), "A:ic_s_row_wifi.spng", net_right,
              HubAction::Network);
    AddHubRow(list, I18n::T("主题"), "A:ic_s_row_theme.spng", theme_right,
              HubAction::Theme);
    AddHubRow(list, I18n::T("音量"), "A:ic_s_row_volume.spng", vol_right,
              HubAction::Volume);
    AddHubRow(list, I18n::T("背光"), "A:ic_s_row_backlight.spng", bright_right,
              HubAction::Backlight);
    AddHubRow(list, I18n::T("OTA 升级"), "A:ic_s_row_ota.spng",
              []() -> const char* {
                  Settings s("ota", false);
                  return s.GetInt("auto_upgrade", 1) != 0 ? I18n::T("开")
                                                           : I18n::T("关");
              }(),
              HubAction::Ota);
    AddHubRow(list, I18n::T("系统信息"), "A:ic_s_row_info.spng", "",
              HubAction::Info);
}

void StyleDetailContainer(lv_obj_t* box) {
    lv_obj_set_size(box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 8, LV_PART_MAIN);
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    screen_swipe_back_ignore(box, true);
}

void ShowDetailVolume() {
    ClearBody();
    s_ui.mode = ViewMode::DetailVolume;
    SetTitle(I18n::T("音量"));
    lv_obj_t* box = lv_obj_create(s_ui.body);
    screen_strip_obj_chrome(box);
    StyleDetailContainer(box);
    BuildVolumeTab(box, ReadInitialVolume());
}

void ShowDetailOta() {
    ClearBody();
    s_ui.mode = ViewMode::DetailOta;
    SetTitle(I18n::T("OTA 升级"));

    lv_obj_t* box = lv_obj_create(s_ui.body);
    screen_strip_obj_chrome(box);
    StyleDetailContainer(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 8, LV_PART_MAIN);

    const esp_app_desc_t* app = esp_app_get_description();
    char ver[64];
    std::snprintf(ver, sizeof(ver), "%s",
                  (app != nullptr && app->version[0] != '\0') ? app->version
                                                              : "--");

    auto make_info_row = [&](const char* left, const char* right) {
        lv_obj_t* row = lv_obj_create(box);
        screen_strip_obj_chrome(row);
        lv_obj_set_size(row, LV_PCT(100), kRowH);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorCard), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 10, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* l = lv_label_create(row);
        lv_label_set_text(l, left);
        lv_obj_set_style_text_color(l, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(l, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* r = lv_label_create(row);
        lv_label_set_text(r, right);
        lv_obj_set_style_text_color(r, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(r, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
        return r;
    };

    make_info_row(I18n::T("当前版本"), ver);

    lv_obj_t* switch_right = nullptr;
    {
        Settings s("ota", false);
        const bool on = s.GetInt("auto_upgrade", 1) != 0;
        lv_obj_t* row = lv_obj_create(box);
        screen_strip_obj_chrome(row);
        lv_obj_set_size(row, LV_PCT(100), kRowH);
        lv_obj_set_style_bg_color(row, lv_color_hex(kColorCard), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(row, 10, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        screen_swipe_back_ignore(row, true);

        lv_obj_t* l = lv_label_create(row);
        lv_label_set_text(l, I18n::T("开机自动升级"));
        lv_obj_set_style_text_color(l, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(l, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);

        switch_right = lv_label_create(row);
        lv_label_set_text(switch_right, on ? I18n::T("开") : I18n::T("关"));
        lv_obj_set_style_text_color(
            switch_right, lv_color_hex(on ? 0x4ADE80 : kColorSubtle),
            LV_PART_MAIN);
        lv_obj_set_style_text_font(switch_right, &font_puhui_20_4,
                                   LV_PART_MAIN);
        lv_obj_align(switch_right, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_remove_flag(switch_right, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                auto* right = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
                Settings s("ota", true);
                const bool on = s.GetInt("auto_upgrade", 1) != 0;
                s.SetInt("auto_upgrade", on ? 0 : 1);
                const bool now_on = !on;
                if (right != nullptr) {
                    lv_label_set_text(right,
                                      now_on ? I18n::T("开") : I18n::T("关"));
                    lv_obj_set_style_text_color(
                        right, lv_color_hex(now_on ? 0x4ADE80 : kColorSubtle),
                        LV_PART_MAIN);
                }
            },
            LV_EVENT_CLICKED, switch_right);
    }

    lv_obj_t* hint = lv_label_create(box);
    lv_label_set_text(
        hint,
        I18n::T("开启后，开机联网检测到新版本时允许自动升级；\n关闭则仅检查不升级。"));
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(hint, 4, LV_PART_MAIN);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
}

void BuildHubBody(lv_obj_t* parent) {
    lv_obj_t* body = lv_obj_create(parent);
    s_ui.body = body;
    screen_strip_obj_chrome(body);
    lv_obj_set_size(body, kPanelSize, kBodyH);
    lv_obj_set_pos(body, 0, kHeaderH);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, kListPad, LV_PART_MAIN);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    ShowHubList();
}

void ExitSettingsToHome() {
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) {
        lv_indev_wait_release(indev);
    }
    screen_nav_set_return_to_settings(false);
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void OnSwipeBack() {
    // OTA 详情：直接回主页，不回设置列表
    if (s_ui.mode == ViewMode::DetailOta) {
        ExitSettingsToHome();
        return;
    }
    if (s_ui.mode != ViewMode::Hub) {
        ShowHubList();
        return;
    }
    ExitSettingsToHome();
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    s_ui = UiState{};
}

}  // namespace

lv_obj_t* SettingsScreen::Create() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui = UiState{};
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);
    BuildHubBody(scr);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    return scr;
}

void SettingsScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: settings_screen");
    } else {
        ESP_LOGI(TAG, "unload: settings_screen");
    }
}

bool SettingsScreen::IsActive() {
    return s_ui.screen != nullptr && lv_screen_active() == s_ui.screen;
}

bool SettingsScreen::IsOtaDetailActive() {
    return IsActive() && s_ui.mode == ViewMode::DetailOta;
}

void SettingsScreen::ExitToHome() {
    ExitSettingsToHome();
}
