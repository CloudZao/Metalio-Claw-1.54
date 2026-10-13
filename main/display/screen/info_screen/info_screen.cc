#include "info_screen.h"
#include "i18n.h"

#include <cstdio>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_log.h>

#include "board.h"
#include "home_screen/home_screen.h"
#include "panel_metrics.h"
#include "screen_util.h"

LV_FONT_DECLARE(font_puhui_20_4);

namespace {

constexpr const char* TAG = "InfoScreen";

constexpr int kPanelSize = 240;
constexpr int kHeaderH = 36;
constexpr int kPad = 6;
constexpr int kRowH = 32;
constexpr int kRowGap = 4;

constexpr uint32_t kColorBg = 0x0E1116;
constexpr uint32_t kColorCard = 0x1B2030;
constexpr uint32_t kColorText = 0xFFFFFF;
constexpr uint32_t kColorLabel = 0x9AA3B2;
constexpr uint32_t kColorValue = 0xE5E7EB;

struct InfoItem {
    const char* label;
    char value[96];
};

struct UiState {
    lv_obj_t* screen = nullptr;
};
UiState s_ui;

void CollectInfoItems(InfoItem* items, int* count) {
    auto& board = Board::GetInstance();
    const auto* app_desc = esp_app_get_description();

    int idx = 0;

    items[idx].label = I18n::T("型号");
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s",
                  "metalio-claw-1.54");
    ++idx;

    items[idx].label = I18n::T("固件版本");
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s",
                  (app_desc != nullptr && app_desc->version[0] != '\0')
                      ? app_desc->version
                      : "--");
    ++idx;

    items[idx].label = I18n::T("电量");
    {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        if (board.GetBatteryLevel(level, charging, discharging)) {
            std::snprintf(items[idx].value, sizeof(items[idx].value), "%d%%",
                          level);
        } else {
            std::snprintf(items[idx].value, sizeof(items[idx].value), "--");
        }
    }
    ++idx;

    items[idx].label = I18n::T("激活");
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s",
                  I18n::T("已激活"));
    ++idx;

    *count = idx;
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

void OnScreenUnloaded(lv_event_t* /*e*/) { s_ui.screen = nullptr; }

void BuildHeader(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelSize, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("系统信息"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
}

lv_obj_t* CreateInfoRow(lv_obj_t* parent, const InfoItem& item) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, kRowH);
    lv_obj_set_style_bg_color(row, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, item.label);
    lv_obj_set_style_text_color(label, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &font_puhui_20_4, LV_PART_MAIN);

    lv_obj_t* value = lv_label_create(row);
    lv_label_set_text(value, item.value);
    lv_obj_set_style_text_color(value, lv_color_hex(kColorLabel), LV_PART_MAIN);
    lv_obj_set_style_text_font(value, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_width(value, 0);
    lv_obj_set_flex_grow(value, 1);
    lv_label_set_long_mode(value, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(
        value, lv_anim_speed_clamped(24, 3000, 60000), LV_PART_MAIN);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    // 「已激活」用绿色
    if (std::strcmp(item.label, I18n::T("激活")) == 0) {
        lv_obj_set_style_text_color(value, lv_color_hex(0x4ADE80), LV_PART_MAIN);
    }

    return row;
}

void BuildInfoList(lv_obj_t* parent) {
    InfoItem items[16];
    int count = 0;
    CollectInfoItems(items, &count);

    lv_obj_t* list = lv_obj_create(parent);
    screen_strip_obj_chrome(list);
    lv_obj_set_size(list, kPanelSize - 2 * kPad, kPanelSize - kHeaderH - kPad);
    lv_obj_set_pos(list, kPad, kHeaderH);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, kRowGap, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(list, kPad, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (int i = 0; i < count; ++i) {
        CreateInfoRow(list, items[i]);
    }
}

}  // namespace

lv_obj_t* InfoScreen::Create() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);
    BuildInfoList(scr);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    return scr;
}

void InfoScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: info_screen");
    } else {
        ESP_LOGI(TAG, "unload: info_screen");
    }
}
