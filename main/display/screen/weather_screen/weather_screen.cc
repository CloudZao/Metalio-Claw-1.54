#include "weather_screen.h"
#include "i18n.h"

#include "Weather.hpp"
#include "home_screen/home_screen.h"
#include "panel_metrics.h"
#include "screen_util.h"
#include "settings.h"
#include "weather_city_list.h"
#include "weather_icon_map.h"

#include "esp_heap_caps.h"
#include "esp_lv_adapter.h"

#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string>

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr int32_t kPanelW = 240;
constexpr int32_t kHeaderH  = 36;
constexpr int32_t kHeaderSidePad = 6;
constexpr int32_t kRegionBtnW = 36;
constexpr int32_t kRefreshBtnW = 36;
constexpr int32_t kHeaderBtnH = 24;
constexpr int32_t kHeaderBtnGap = 8;
constexpr int32_t kModalCardW = 220;
constexpr int32_t kModalCardPad = 12;
constexpr int32_t kModalDdW = kModalCardW - kModalCardPad * 2;
constexpr int32_t kModalDdH = 32;
constexpr int32_t kTabBarH = 22;
constexpr int32_t kTabPad  = 4;
constexpr int32_t kInnerW = kPanelW - kTabPad * 2;

// 240 屏天气图标资源为 24×24 SPNG（切片格式不可 zoom），必须 1:1 绘制。
constexpr int32_t kWeatherIconSrcPx  = 24;
constexpr int32_t kHeroIconSize      = kWeatherIconSrcPx;
constexpr int32_t kForecastIconSize  = kWeatherIconSrcPx;
constexpr int32_t kHoursIconSize     = kWeatherIconSrcPx;
// 6 日预报：横滑 2 页 × 每页 3 列（约 74px 卡宽），避免一行 6 列过挤。
constexpr int32_t kForecastCols      = 3;
constexpr int32_t kForecastDayCount  = 6;
constexpr int32_t kForecastPages     = kForecastDayCount / kForecastCols;
constexpr int32_t kForecastDotH      = 14;
constexpr int32_t kForecastDotSize   = 6;
constexpr int32_t kForecastGridPadH  = 2;
constexpr int32_t kForecastColGap    = 4;
constexpr int32_t kForecastCardPad   = 4;
constexpr int32_t kForecastUsableW =
    kInnerW - kForecastGridPadH * 2;
constexpr int32_t kForecastCardW =
    (kForecastUsableW - kForecastColGap * (kForecastCols - 1)) / kForecastCols;
constexpr int32_t kForecastCardH = LV_PCT(100);

// 生活指数：横滑分页，每页左右 2 卡，最多 4 条。
constexpr int32_t kIndexPerPage   = 2;
constexpr int32_t kIndexMaxCount  = 4;
constexpr int32_t kIndexDotH      = kForecastDotH;
constexpr int32_t kIndexDotSize   = kForecastDotSize;
constexpr int32_t kIndexGap       = 4;

constexpr uint32_t kColorBg         = 0x0E1116;
constexpr uint32_t kColorBgGrad     = 0x161A22;
constexpr uint32_t kColorCard       = 0x2A2F3A;
constexpr uint32_t kColorCardAccent = 0xE0FB3C;
constexpr uint32_t kColorText       = 0xFFFFFF;
constexpr uint32_t kColorSubtle     = 0x9AA3B2;
constexpr uint32_t kColorAccent     = 0x60A5FA;
constexpr uint32_t kColorTabActive  = 0x3B82F6;
constexpr uint32_t kColorHeaderBg   = 0x12151C;
constexpr uint32_t kColorDivider    = 0x2A2F3A;
constexpr uint32_t kColorHeaderBtn  = 0x2A2F3A;
constexpr uint32_t kColorHeaderBtnBorder = 0x3B4556;

lv_obj_t* s_screen       = nullptr;
lv_obj_t* s_tabview      = nullptr;
lv_obj_t* s_tab_overview  = nullptr;
lv_obj_t* s_tab_forecast  = nullptr;
lv_obj_t* s_tab_hours     = nullptr;
lv_obj_t* s_tab_index     = nullptr;
lv_obj_t* s_status_lbl   = nullptr;
lv_obj_t* s_prov_dd      = nullptr;
lv_obj_t* s_city_dd      = nullptr;
lv_obj_t* s_dist_dd      = nullptr;
lv_obj_t* s_city_dlg_mask = nullptr;
std::string s_prov_options;
std::string s_city_options;
std::string s_dist_options;
bool s_dd_syncing        = false;
std::atomic<uint32_t> s_session{0};

const lv_font_t* font_main() { return &font_puhui_20_4; }
const lv_font_t* font_sub()  { return &font_puhui_16_4; }
const lv_font_t* font_tiny() { return &font_puhui_16_4; }

void TriggerFetch();

void StyleDropdown(lv_obj_t* dd) {
    lv_obj_set_size(dd, kModalDdW, kModalDdH);
    lv_obj_set_style_radius(dd, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dd, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dd, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(dd, lv_color_hex(0x4B5563), LV_PART_MAIN);
    lv_obj_set_style_text_color(dd, lv_color_hex(kColorText), LV_PART_MAIN);
    // 选项文案：puhui 16_4；箭头：Montserrat（勿用 ""，会被当成图片源解 SPNG 崩）
    lv_obj_set_style_text_font(dd, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_14, LV_PART_INDICATOR);
    lv_obj_set_style_pad_left(dd, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(dd, 24, LV_PART_MAIN);
    lv_dropdown_set_symbol(dd, LV_SYMBOL_DOWN);
    screen_swipe_back_ignore(dd, true);
}

void StyleHeaderBtn(lv_obj_t* btn) {
    lv_obj_set_style_radius(btn, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(kColorHeaderBtn), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(kColorHeaderBtnBorder),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3B4556),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    screen_swipe_back_ignore(btn, true);
}

void OnDropdownListOpened(lv_event_t* e) {
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
    lv_obj_set_style_text_font(list, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_radius(list, 10, LV_PART_MAIN);
    lv_obj_set_style_max_height(list, 120, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(kColorTabActive),
                              static_cast<lv_part_t>(LV_PART_SELECTED) |
                                  static_cast<lv_state_t>(LV_STATE_CHECKED));
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER,
                            static_cast<lv_part_t>(LV_PART_SELECTED) |
                                static_cast<lv_state_t>(LV_STATE_CHECKED));
    screen_swipe_back_ignore(list, true);
}

void AppendDropdownOption(std::string& buf, const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    if (!buf.empty()) {
        buf += '\n';
    }
    buf += name;
}

void FillProvinceOptions() {
    if (s_prov_dd == nullptr) {
        return;
    }
    s_prov_options.clear();
    for (size_t i = 0; i < weather_cities::kProvinceCount; ++i) {
        AppendDropdownOption(s_prov_options, weather_cities::kProvinces[i].name);
    }
    lv_dropdown_set_options(s_prov_dd, s_prov_options.c_str());
}

void FillCityOptions(size_t province_idx) {
    if (s_city_dd == nullptr || province_idx >= weather_cities::kProvinceCount) {
        return;
    }
    const weather_cities::ProvinceEntry& pe =
        weather_cities::kProvinces[province_idx];
    s_city_options.clear();
    for (uint16_t i = 0; i < pe.city_count; ++i) {
        AppendDropdownOption(s_city_options,
                             weather_cities::kCities[pe.city_start + i].name);
    }
    lv_dropdown_set_options(s_city_dd, s_city_options.c_str());
}

void FillDistrictOptions(size_t province_idx, size_t city_idx) {
    if (s_dist_dd == nullptr || province_idx >= weather_cities::kProvinceCount) {
        return;
    }
    const weather_cities::ProvinceEntry& pe =
        weather_cities::kProvinces[province_idx];
    if (city_idx >= pe.city_count) {
        return;
    }
    const weather_cities::CityEntry& ce =
        weather_cities::kCities[pe.city_start + city_idx];
    s_dist_options.clear();
    for (uint16_t i = 0; i < ce.district_count; ++i) {
        AppendDropdownOption(s_dist_options,
                             weather_cities::kDistricts[ce.district_start + i].name);
    }
    lv_dropdown_set_options(s_dist_dd, s_dist_options.c_str());
}

std::string LoadSavedDistrictId() {
    Settings settings("weather", false);
    std::string id =
        settings.GetString("district_id", WeatherService::kDefaultDistrictId);
    if (weather_cities::FindDistrictById(id.c_str()).province_idx >=
        weather_cities::kProvinceCount) {
        id = WeatherService::kDefaultDistrictId;
    }
    return id;
}

void SaveDistrictId(const char* district_id) {
    if (district_id == nullptr || district_id[0] == '\0') {
        return;
    }
    Settings settings("weather", true);
    settings.SetString("district_id", district_id);
}

const char* SelectedDistrictId() {
    if (s_prov_dd == nullptr || s_city_dd == nullptr || s_dist_dd == nullptr) {
        return WeatherService::kDefaultDistrictId;
    }
    const size_t prov = lv_dropdown_get_selected(s_prov_dd);
    const size_t city = lv_dropdown_get_selected(s_city_dd);
    const size_t dist = lv_dropdown_get_selected(s_dist_dd);
    const weather_cities::DistrictEntry* entry =
        weather_cities::GetDistrict(prov, city, dist);
    if (entry == nullptr) {
        return WeatherService::kDefaultDistrictId;
    }
    return entry->id;
}

void CloseCityPickerDialog() {
    if (s_city_dlg_mask != nullptr) {
        lv_obj_delete(s_city_dlg_mask);
        s_city_dlg_mask = nullptr;
    }
    s_prov_dd = nullptr;
    s_city_dd = nullptr;
    s_dist_dd = nullptr;
}

void SyncDropdownsFromDistrictId(const char* district_id) {
    if (s_prov_dd == nullptr || s_city_dd == nullptr || s_dist_dd == nullptr) {
        return;
    }
    const weather_cities::LookupResult found =
        weather_cities::FindDistrictById(district_id);
    size_t prov = found.province_idx;
    size_t city = found.city_idx;
    size_t dist = found.district_idx;
    if (prov >= weather_cities::kProvinceCount) {
        const weather_cities::LookupResult fallback =
            weather_cities::FindDistrictById(WeatherService::kDefaultDistrictId);
        prov = fallback.province_idx;
        city = fallback.city_idx;
        dist = fallback.district_idx;
    }

    s_dd_syncing = true;
    FillProvinceOptions();
    lv_dropdown_set_selected(s_prov_dd, static_cast<uint32_t>(prov));
    FillCityOptions(prov);
    lv_dropdown_set_selected(s_city_dd, static_cast<uint32_t>(city));
    FillDistrictOptions(prov, city);
    lv_dropdown_set_selected(s_dist_dd, static_cast<uint32_t>(dist));
    s_dd_syncing = false;
}

void OnProvinceChanged(lv_event_t* /*e*/) {
    if (s_dd_syncing || s_prov_dd == nullptr) {
        return;
    }
    const size_t prov = lv_dropdown_get_selected(s_prov_dd);
    s_dd_syncing = true;
    FillCityOptions(prov);
    lv_dropdown_set_selected(s_city_dd, 0);
    FillDistrictOptions(prov, 0);
    lv_dropdown_set_selected(s_dist_dd, 0);
    s_dd_syncing = false;
}

void OnCityChanged(lv_event_t* /*e*/) {
    if (s_dd_syncing || s_prov_dd == nullptr || s_city_dd == nullptr) {
        return;
    }
    const size_t prov = lv_dropdown_get_selected(s_prov_dd);
    const size_t city = lv_dropdown_get_selected(s_city_dd);
    s_dd_syncing = true;
    FillDistrictOptions(prov, city);
    lv_dropdown_set_selected(s_dist_dd, 0);
    s_dd_syncing = false;
}

void OnDistrictChanged(lv_event_t* /*e*/) {}

void OnCityPickerCancelClicked(lv_event_t* /*e*/) { CloseCityPickerDialog(); }

void OnCityPickerMaskClicked(lv_event_t* e) {
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    CloseCityPickerDialog();
}

void OnCityPickerSaveClicked(lv_event_t* /*e*/) {
    const char* district_id = SelectedDistrictId();
    SaveDistrictId(district_id);
    WeatherService::Instance().SetDistrictId(district_id);
    CloseCityPickerDialog();
    TriggerFetch();
}

void OpenCityPickerDialog() {
    if (s_screen == nullptr || s_city_dlg_mask != nullptr) {
        return;
    }

    constexpr int32_t kBtnW = 80;
    constexpr int32_t kBtnH = 28;
    constexpr int32_t kBtnRowH = 40;  // pad_top 12 + kBtnH 28，避免按钮底边被裁切

    lv_obj_t* mask = lv_obj_create(s_screen);
    s_city_dlg_mask = mask;
    screen_strip_obj_chrome(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelW, kPanelW);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    lv_obj_add_event_cb(mask, OnCityPickerMaskClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_width(card, kModalCardW);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, kModalCardPad, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 4, LV_PART_MAIN);

    lv_obj_t* dlg_title = lv_label_create(card);
    lv_label_set_text(dlg_title, I18n::T("选择地区"));
    lv_obj_set_style_text_color(dlg_title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(dlg_title, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_text_align(dlg_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(dlg_title, LV_PCT(100));
    lv_obj_set_style_pad_bottom(dlg_title, 4, LV_PART_MAIN);
    lv_obj_remove_flag(dlg_title, LV_OBJ_FLAG_CLICKABLE);

    auto make_picker_row = [&](const char* label, lv_obj_t** out_dd) {
        lv_obj_t* row = lv_obj_create(card);
        screen_strip_obj_chrome(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(row, 2, LV_PART_MAIN);

        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, font_sub(), LV_PART_MAIN);
        lv_obj_set_style_min_width(lbl, 36, LV_PART_MAIN);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        *out_dd = lv_dropdown_create(row);
        StyleDropdown(*out_dd);
        // 行内标签占宽后，下拉吃剩余空间（勿用整卡宽 kModalDdW）
        lv_obj_set_width(*out_dd, 1);
        lv_obj_set_flex_grow(*out_dd, 1);
        lv_obj_add_event_cb(*out_dd, OnDropdownListOpened, LV_EVENT_READY, nullptr);
    };

    make_picker_row(I18n::T("省份"), &s_prov_dd);
    lv_obj_add_event_cb(s_prov_dd, OnProvinceChanged, LV_EVENT_VALUE_CHANGED,
                        nullptr);

    make_picker_row(I18n::T("城市"), &s_city_dd);
    lv_obj_add_event_cb(s_city_dd, OnCityChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    make_picker_row(I18n::T("区县"), &s_dist_dd);
    lv_obj_add_event_cb(s_dist_dd, OnDistrictChanged, LV_EVENT_VALUE_CHANGED,
                        nullptr);

    SyncDropdownsFromDistrictId(WeatherService::Instance().DistrictId().c_str());

    lv_obj_t* btn_row = lv_obj_create(card);
    screen_strip_obj_chrome(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, kBtnRowH);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(btn_row, 12, LV_PART_MAIN);

    lv_obj_t* cancel = lv_button_create(btn_row);
    lv_obj_set_size(cancel, kBtnW, kBtnH);
    StyleHeaderBtn(cancel);
    lv_obj_add_event_cb(cancel, OnCityPickerCancelClicked, LV_EVENT_CLICKED,
                        nullptr);
    lv_obj_t* cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, I18n::T("取消"));
    lv_obj_set_style_text_font(cancel_lbl, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_center(cancel_lbl);
    lv_obj_remove_flag(cancel_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* save = lv_button_create(btn_row);
    lv_obj_set_size(save, kBtnW, kBtnH);
    lv_obj_set_style_radius(save, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(save, lv_color_hex(kColorTabActive), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(save, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(save, 0, LV_PART_MAIN);
    screen_swipe_back_ignore(save, true);
    lv_obj_add_event_cb(save, OnCityPickerSaveClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* save_lbl = lv_label_create(save);
    lv_label_set_text(save_lbl, I18n::T("保存"));
    lv_obj_set_style_text_font(save_lbl, font_sub(), LV_PART_MAIN);
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_center(save_lbl);
    lv_obj_remove_flag(save_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void OnCityPickerOpenClicked(lv_event_t* /*e*/) { OpenCityPickerDialog(); }

void SetWeatherIcon(lv_obj_t* icon, const std::string& text, int32_t display_size) {
    if (icon == nullptr) {
        return;
    }
    const char* code = WeatherIconCodeForText(text);
    if (code == nullptr) {
        lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // SPNG 切片不可缩放：显示尺寸必须等于资源像素，否则花屏/棋盘格。
    (void)display_size;
    const int32_t draw = kWeatherIconSrcPx;
    char path[48];
    std::snprintf(path, sizeof(path), "A:ic_s_weather_%s.spng", code);
    lv_obj_set_size(icon, draw, draw);
    lv_image_set_src(icon, path);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font,
                    uint32_t color, lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, align, LV_PART_MAIN);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    screen_make_input_passive(lbl);
    return lbl;
}

// 过长文案循环横滚；约 16px/s，比默认慢一截，便于阅读
void SetLabelMarquee(lv_obj_t* lbl) {
    if (lbl == nullptr) {
        return;
    }
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(
        lbl, lv_anim_speed_clamped(16, 3000, 60000), LV_PART_MAIN);
}

void ClearContainer(lv_obj_t* container) {
    if (container == nullptr) {
        return;
    }
    while (lv_obj_get_child_count(container) > 0) {
        lv_obj_delete(lv_obj_get_child(container, 0));
    }
}

void StyleScrollTab(lv_obj_t* tab) {
    lv_obj_set_style_pad_all(tab, kTabPad, LV_PART_MAIN);
    lv_obj_set_style_pad_row(tab, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, LV_PART_MAIN);
    // 240 一屏布局：禁止竖滚，避免「还要往下翻」
    lv_obj_remove_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
}

lv_obj_t* CreateForecastCard(lv_obj_t* parent, const WeatherForecastDay& day,
                             int index) {
    const bool accent = (index % 2 == 1);
    const uint32_t bg = accent ? kColorCardAccent : kColorCard;
    const uint32_t fg = accent ? 0x1A1A1A : kColorText;
    const uint32_t fg2 = accent ? 0x404040 : kColorSubtle;

    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, kForecastCardW, kForecastCardH);
    screen_strip_obj_chrome(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, kForecastCardPad, LV_PART_MAIN);
    lv_obj_set_style_pad_top(card, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(card, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(card, true, LV_PART_MAIN);

    const int32_t text_w = kForecastCardW - kForecastCardPad * 2;

    lv_obj_t* week_lbl =
        MakeLabel(card, day.week.empty() ? "--" : I18n::T(day.week.c_str()),
                  font_tiny(), fg, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(week_lbl, text_w);

    char date_buf[16];
    if (day.date.size() >= 10) {
        const int month = std::atoi(day.date.substr(5, 2).c_str());
        const int day_num = std::atoi(day.date.substr(8, 2).c_str());
        std::snprintf(date_buf, sizeof(date_buf), "%d/%d", month, day_num);
    } else {
        std::snprintf(date_buf, sizeof(date_buf), "--");
    }
    lv_obj_t* date_lbl =
        MakeLabel(card, date_buf, font_tiny(), fg2, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(date_lbl, text_w);

    lv_obj_t* icon = lv_image_create(card);
    SetWeatherIcon(icon, day.text_day, kForecastIconSize);
    screen_make_input_passive(icon);

    char temp_buf[32];
    std::snprintf(temp_buf, sizeof(temp_buf), "%" PRId32 "~%" PRId32 "°",
                  day.low, day.high);
    lv_obj_t* temp_lbl =
        MakeLabel(card, temp_buf, font_tiny(), fg, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(temp_lbl, text_w);

    // 白天描述；极端长文案（如雷阵雨伴有冰雹）仍循环滚动
    lv_obj_t* day_lbl = MakeLabel(card, I18n::T(day.text_day.c_str()), font_tiny(),
                                  fg2, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(day_lbl, text_w);
    SetLabelMarquee(day_lbl);

    screen_make_input_passive(card);
    return card;
}

void UpdatePagerDots(lv_obj_t* dots, int page) {
    if (dots == nullptr) {
        return;
    }
    const uint32_t n = lv_obj_get_child_count(dots);
    if (n == 0) {
        return;
    }
    if (page < 0) {
        page = 0;
    }
    if (page >= static_cast<int>(n)) {
        page = static_cast<int>(n) - 1;
    }
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* dot = lv_obj_get_child(dots, i);
        if (dot == nullptr) {
            continue;
        }
        const bool on = (static_cast<int>(i) == page);
        lv_obj_set_style_bg_color(
            dot, lv_color_hex(on ? kColorAccent : kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, on ? LV_OPA_COVER : LV_OPA_60, LV_PART_MAIN);
    }
}

void OnPagerScrollEnd(lv_event_t* e) {
    lv_obj_t* pager = lv_event_get_target_obj(e);
    lv_obj_t* dots = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    if (pager == nullptr || dots == nullptr) {
        return;
    }
    const int32_t page_w = lv_obj_get_width(pager);
    if (page_w <= 0) {
        return;
    }
    const int32_t scroll_x = lv_obj_get_scroll_x(pager);
    const int page = static_cast<int>((scroll_x + page_w / 2) / page_w);
    UpdatePagerDots(dots, page);
}

void ShowStatus(const char* text) {
    if (s_status_lbl != nullptr) {
        lv_label_set_text(s_status_lbl, text);
    }
}

void BuildOverviewTab(const WeatherDistrictData& data) {
    if (s_tab_overview == nullptr) {
        return;
    }
    ClearContainer(s_tab_overview);

    // 左右分栏：左图标气温，右湿度/风力/体感（对齐 HTML 一屏布局）
    lv_obj_t* row = lv_obj_create(s_tab_overview);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_PCT(100));
    screen_strip_obj_chrome(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);

    lv_obj_t* hero = lv_obj_create(row);
    screen_strip_obj_chrome(hero);
    lv_obj_set_size(hero, 88, LV_PCT(100));
    lv_obj_set_style_bg_color(hero, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hero, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(hero, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(hero, 4, LV_PART_MAIN);
    lv_obj_remove_flag(hero, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(hero, 2, LV_PART_MAIN);

    lv_obj_t* icon = lv_image_create(hero);
    SetWeatherIcon(icon, data.text, kHeroIconSize);
    screen_make_input_passive(icon);

    char temp_line[24];
    std::snprintf(temp_line, sizeof(temp_line), "%" PRId32 "°", data.temp);
    MakeLabel(hero, temp_line, font_main(), kColorText, LV_TEXT_ALIGN_CENTER);

    char meta[48];
    // 避免中文间隔号「·」缺字成灰块
    std::snprintf(meta, sizeof(meta), "%s/%s", I18n::T(data.text.c_str()),
                  data.city.empty() ? data.district.c_str() : data.city.c_str());
    lv_obj_t* meta_lbl =
        MakeLabel(hero, meta, font_sub(), kColorSubtle, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(meta_lbl, 80);
    lv_label_set_long_mode(meta_lbl, LV_LABEL_LONG_CLIP);
    screen_make_input_passive(hero);

    lv_obj_t* side = lv_obj_create(row);
    screen_strip_obj_chrome(side);
    lv_obj_set_flex_grow(side, 1);
    lv_obj_set_height(side, LV_PCT(100));
    lv_obj_set_style_bg_opa(side, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(side, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(side, 4, LV_PART_MAIN);

    auto add_side_card = [&](const char* title, const char* value) {
        lv_obj_t* card = lv_obj_create(side);
        screen_strip_obj_chrome(card);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 6, LV_PART_MAIN);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        MakeLabel(card, title, font_sub(), kColorSubtle);
        MakeLabel(card, value, font_sub(), kColorText);
        screen_make_input_passive(card);
    };

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d%% %s %s", static_cast<int>(data.rh),
                  I18n::T(data.wind_dir.c_str()),
                  I18n::T(data.wind_class.c_str()));
    add_side_card(I18n::T("湿度 / 风力"), buf);

    int32_t lo = data.temp;
    int32_t hi = data.temp;
    if (!data.forecasts.empty()) {
        lo = data.forecasts[0].low;
        hi = data.forecasts[0].high;
    }
    std::snprintf(buf, sizeof(buf), "%" PRId32 "°~%" PRId32 "°", lo, hi);
    add_side_card(I18n::T("今日气温"), buf);

    std::snprintf(buf, sizeof(buf), "%" PRId32 "° UV%" PRId32, data.feels_like,
                  data.uvi);
    add_side_card(I18n::T("体感 / 紫外线"), buf);

    screen_make_input_passive(row);
}

void BuildForecastTab(const WeatherDistrictData& data) {
    if (s_tab_forecast == nullptr) {
        return;
    }
    ClearContainer(s_tab_forecast);

    if (data.forecasts.size() <= 1) {
        MakeLabel(s_tab_forecast, I18n::T("暂无6日预报"), font_main(), kColorSubtle,
                  LV_TEXT_ALIGN_CENTER);
        return;
    }

    const size_t start = 1;
    const size_t end = std::min(
        data.forecasts.size(),
        start + static_cast<size_t>(kForecastDayCount));

    lv_obj_update_layout(s_tab_forecast);
    int32_t avail_w = lv_obj_get_content_width(s_tab_forecast);
    int32_t avail_h = lv_obj_get_content_height(s_tab_forecast);
    if (avail_w < 8) {
        avail_w = kInnerW;
    }
    if (avail_h < 8) {
        avail_h = kPanelW - kHeaderH - kTabBarH - kTabPad * 2;
    }
    const int32_t page_h = avail_h - kForecastDotH - 4;
    const int32_t page_w = avail_w;
    if (page_h < 40 || page_w < 40) {
        MakeLabel(s_tab_forecast, I18n::T("暂无6日预报"), font_main(), kColorSubtle,
                  LV_TEXT_ALIGN_CENTER);
        return;
    }

    lv_obj_t* pager = lv_obj_create(s_tab_forecast);
    screen_strip_obj_chrome(pager);
    lv_obj_set_size(pager, page_w, page_h);
    lv_obj_set_style_bg_opa(pager, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pager, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(pager, LV_OBJ_FLAG_SCROLL_ONE);
    screen_swipe_back_ignore(pager, true);

    for (int page = 0; page < kForecastPages; ++page) {
        lv_obj_t* page_obj = lv_obj_create(pager);
        screen_strip_obj_chrome(page_obj);
        lv_obj_set_size(page_obj, page_w, page_h);
        lv_obj_set_pos(page_obj, page * page_w, 0);
        lv_obj_set_style_bg_opa(page_obj, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(page_obj, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* fc_row = lv_obj_create(page_obj);
        lv_obj_set_size(fc_row, LV_PCT(100), LV_PCT(100));
        screen_strip_obj_chrome(fc_row);
        lv_obj_set_style_bg_opa(fc_row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(fc_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(fc_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(fc_row, kForecastColGap, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(fc_row, kForecastGridPadH, LV_PART_MAIN);
        lv_obj_set_flex_align(fc_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        for (int col = 0; col < kForecastCols; ++col) {
            const size_t idx =
                start + static_cast<size_t>(page * kForecastCols + col);
            if (idx >= end) {
                break;
            }
            CreateForecastCard(fc_row, data.forecasts[idx],
                               page * kForecastCols + col);
        }
        screen_make_input_passive(fc_row);
        screen_make_input_passive(page_obj);
    }

    lv_obj_t* dots = lv_obj_create(s_tab_forecast);
    screen_strip_obj_chrome(dots);
    lv_obj_set_size(dots, LV_PCT(100), kForecastDotH);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dots, 8, LV_PART_MAIN);
    screen_make_input_passive(dots);

    for (int i = 0; i < kForecastPages; ++i) {
        lv_obj_t* dot = lv_obj_create(dots);
        screen_strip_obj_chrome(dot);
        lv_obj_set_size(dot, kForecastDotSize, kForecastDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        screen_make_input_passive(dot);
    }
    UpdatePagerDots(dots, 0);

    lv_obj_add_event_cb(pager, OnPagerScrollEnd, LV_EVENT_SCROLL_END,
                        dots);
}

void BuildIndexTab(const WeatherDistrictData& data) {
    if (s_tab_index == nullptr) {
        return;
    }
    ClearContainer(s_tab_index);

    if (data.indexes.empty()) {
        MakeLabel(s_tab_index, I18n::T("暂无生活指数"), font_main(), kColorSubtle,
                  LV_TEXT_ALIGN_CENTER);
        return;
    }

    const size_t n =
        std::min(data.indexes.size(), static_cast<size_t>(kIndexMaxCount));
    const int pages = static_cast<int>(
        (n + static_cast<size_t>(kIndexPerPage) - 1) /
        static_cast<size_t>(kIndexPerPage));

    lv_obj_update_layout(s_tab_index);
    int32_t avail_w = lv_obj_get_content_width(s_tab_index);
    int32_t avail_h = lv_obj_get_content_height(s_tab_index);
    if (avail_w < 8) {
        avail_w = kInnerW;
    }
    if (avail_h < 8) {
        avail_h = kPanelW - kHeaderH - kTabBarH - kTabPad * 2;
    }
    const bool show_dots = (pages > 1);
    const int32_t page_h =
        show_dots ? (avail_h - kIndexDotH - 4) : avail_h;
    const int32_t page_w = avail_w;
    if (page_h < 40 || page_w < 40) {
        MakeLabel(s_tab_index, I18n::T("暂无生活指数"), font_main(), kColorSubtle,
                  LV_TEXT_ALIGN_CENTER);
        return;
    }

    const int32_t card_w = (page_w - kIndexGap) / kIndexPerPage;
    const int32_t card_h = page_h;

    lv_obj_t* pager = lv_obj_create(s_tab_index);
    screen_strip_obj_chrome(pager);
    lv_obj_set_size(pager, page_w, page_h);
    lv_obj_set_style_bg_opa(pager, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pager, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(pager, LV_OBJ_FLAG_SCROLL_ONE);
    screen_swipe_back_ignore(pager, true);

    for (int page = 0; page < pages; ++page) {
        lv_obj_t* page_obj = lv_obj_create(pager);
        screen_strip_obj_chrome(page_obj);
        lv_obj_set_size(page_obj, page_w, page_h);
        lv_obj_set_pos(page_obj, page * page_w, 0);
        lv_obj_set_style_bg_opa(page_obj, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(page_obj, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* row = lv_obj_create(page_obj);
        lv_obj_set_size(row, LV_PCT(100), LV_PCT(100));
        screen_strip_obj_chrome(row);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, kIndexGap, LV_PART_MAIN);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        for (int col = 0; col < kIndexPerPage; ++col) {
            const size_t idx =
                static_cast<size_t>(page * kIndexPerPage + col);
            if (idx >= n) {
                break;
            }
            const auto& item = data.indexes[idx];
            lv_obj_t* card = lv_obj_create(row);
            lv_obj_set_size(card, card_w, card_h);
            screen_strip_obj_chrome(card);
            lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard),
                                     LV_PART_MAIN);
            lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
            lv_obj_set_style_pad_all(card, 6, LV_PART_MAIN);
            lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                                  LV_FLEX_ALIGN_START);
            lv_obj_set_style_pad_row(card, 4, LV_PART_MAIN);
            lv_obj_set_style_clip_corner(card, true, LV_PART_MAIN);

            char title[48];
            if (item.brief.empty()) {
                std::snprintf(title, sizeof(title), "%s", item.name.c_str());
            } else {
                std::snprintf(title, sizeof(title), "%s/%s", item.name.c_str(),
                              item.brief.c_str());
            }
            lv_obj_t* t = MakeLabel(card, title, font_sub(), kColorText);
            lv_obj_set_width(t, LV_PCT(100));
            lv_label_set_long_mode(t, LV_LABEL_LONG_CLIP);
            if (!item.detail.empty()) {
                lv_obj_t* d = MakeLabel(card, item.detail.c_str(), font_sub(),
                                        kColorSubtle);
                lv_obj_set_width(d, LV_PCT(100));
                lv_obj_set_flex_grow(d, 1);
                lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
            }
            screen_make_input_passive(card);
        }
        screen_make_input_passive(row);
        screen_make_input_passive(page_obj);
    }

    if (!show_dots) {
        return;
    }

    lv_obj_t* dots = lv_obj_create(s_tab_index);
    screen_strip_obj_chrome(dots);
    lv_obj_set_size(dots, LV_PCT(100), kIndexDotH);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dots, 8, LV_PART_MAIN);
    screen_make_input_passive(dots);

    for (int i = 0; i < pages; ++i) {
        lv_obj_t* dot = lv_obj_create(dots);
        screen_strip_obj_chrome(dot);
        lv_obj_set_size(dot, kIndexDotSize, kIndexDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        screen_make_input_passive(dot);
    }
    UpdatePagerDots(dots, 0);

    lv_obj_add_event_cb(pager, OnPagerScrollEnd, LV_EVENT_SCROLL_END, dots);
}

void BuildHoursTab(const WeatherDistrictData& data) {
    if (s_tab_hours == nullptr) {
        return;
    }
    ClearContainer(s_tab_hours);

    if (data.forecast_hours.empty()) {
        MakeLabel(s_tab_hours, I18n::T("暂无24小时预报"), font_main(), kColorSubtle,
                  LV_TEXT_ALIGN_CENTER);
        return;
    }

    lv_obj_t* grid = lv_obj_create(s_tab_hours);
    lv_obj_set_size(grid, LV_PCT(100), LV_PCT(100));
    screen_strip_obj_chrome(grid);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(grid, 4, LV_PART_MAIN);

    constexpr int32_t kHourCardW = (kInnerW - 8) / 3;
    constexpr int32_t kHourCardH = 78;

    const size_t n = std::min(data.forecast_hours.size(), static_cast<size_t>(6));
    for (size_t i = 0; i < n; ++i) {
        const auto& hour = data.forecast_hours[i];
        lv_obj_t* card = lv_obj_create(grid);
        lv_obj_set_size(card, kHourCardW, kHourCardH);
        screen_strip_obj_chrome(card);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 4, LV_PART_MAIN);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(card, 0, LV_PART_MAIN);
        lv_obj_set_style_clip_corner(card, true, LV_PART_MAIN);

        std::string time_str = hour.data_time;
        if (time_str.size() >= 16) {
            time_str = time_str.substr(11, 5);
        }
        MakeLabel(card, time_str.c_str(), font_tiny(), kColorSubtle,
                  LV_TEXT_ALIGN_CENTER);

        lv_obj_t* icon = lv_image_create(card);
        SetWeatherIcon(icon, hour.text, kHoursIconSize);
        screen_make_input_passive(icon);

        char temp_buf[16];
        std::snprintf(temp_buf, sizeof(temp_buf), "%" PRId32 "°", hour.temp_fc);
        MakeLabel(card, temp_buf, font_tiny(), kColorAccent, LV_TEXT_ALIGN_CENTER);
        lv_obj_t* txt = MakeLabel(card, I18n::T(hour.text.c_str()), font_tiny(),
                                  kColorText, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(txt, kHourCardW - 8);
        SetLabelMarquee(txt);
        screen_make_input_passive(card);
    }
    screen_make_input_passive(grid);
}

void BuildWeatherUi(const WeatherDistrictData& data) {
    BuildOverviewTab(data);
    BuildForecastTab(data);
    BuildHoursTab(data);
    BuildIndexTab(data);
}

void ClearTabMessage(lv_obj_t* tab, const char* msg) {
    if (tab == nullptr) {
        return;
    }
    ClearContainer(tab);
    if (msg != nullptr && msg[0] != '\0') {
        MakeLabel(tab, msg, font_main(), kColorSubtle, LV_TEXT_ALIGN_CENTER);
    }
}

void ApplyWeatherData(const WeatherDistrictData& data) {
    if (data.valid) {
        BuildWeatherUi(data);
        ShowStatus("");
    } else {
        ClearTabMessage(s_tab_overview, I18n::T("暂无天气数据"));
        ClearTabMessage(s_tab_forecast, nullptr);
        ClearTabMessage(s_tab_hours, nullptr);
        ClearTabMessage(s_tab_index, nullptr);
        ShowStatus(I18n::T("加载失败"));
    }
}

void ShowLoadingPlaceholder() {
    ClearTabMessage(s_tab_overview, I18n::T("加载中..."));
    ClearTabMessage(s_tab_forecast, nullptr);
    ClearTabMessage(s_tab_hours, nullptr);
    ClearTabMessage(s_tab_index, nullptr);
}

void FetchTask(void* arg) {
    const uint32_t my_session = reinterpret_cast<uintptr_t>(arg);
    WeatherDistrictData data;
    const esp_err_t err = WeatherService::Instance().Fetch(data);

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        if (s_session.load() == my_session && s_screen != nullptr) {
            if (err == ESP_OK) {
                ApplyWeatherData(data);
            } else {
                ApplyWeatherData(WeatherDistrictData{});
                const std::string& err_msg = WeatherService::Instance().LastError();
                ShowStatus(err_msg.empty() ? I18n::T("加载失败")
                                           : I18n::T(err_msg.c_str()));
            }
        }
        esp_lv_adapter_unlock();
    }
    vTaskDeleteWithCaps(nullptr);
}

void TriggerFetch() {
    const uint32_t session =
        s_session.fetch_add(1, std::memory_order_relaxed) + 1;
    ShowStatus(I18n::T("加载中..."));
    ShowLoadingPlaceholder();

    // 12KB 栈 + HTTP/JSON 解析；内部 DRAM 碎片化时 xTaskCreate 会失败或栈溢出。
    constexpr UBaseType_t kFetchStackCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    BaseType_t ok = xTaskCreateWithCaps(
        FetchTask, "weather_fetch", 12 * 1024,
        reinterpret_cast<void*>(static_cast<uintptr_t>(session)), 4, nullptr,
        kFetchStackCaps);
    if (ok != pdPASS) {
        ESP_LOGE("WeatherScreen",
                 "create fetch task failed (internal=%uKB largest=%uKB psram=%uKB)",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) /
                            1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        ClearTabMessage(s_tab_overview, I18n::T("加载失败"));
        ShowStatus(I18n::T("任务创建失败"));
    }
}

void OnSwipeBack();

void OnRefreshClicked(lv_event_t* /*e*/) { TriggerFetch(); }

void BuildTabView(lv_obj_t* scr) {
    const int32_t body_h = kPanelW - kHeaderH;

    lv_obj_t* tv = lv_tabview_create(scr);
    s_tabview = tv;
    lv_obj_set_size(tv, kPanelW, body_h);
    lv_obj_set_pos(tv, 0, kHeaderH);
    lv_tabview_set_tab_bar_position(tv, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv, kTabBarH);
    lv_obj_set_style_bg_color(tv, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tv, 0, LV_PART_MAIN);

    lv_obj_t* bar = lv_tabview_get_tab_bar(tv);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_column(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(bar, font_sub(), LV_PART_MAIN);
    // pill 分段：未选透明，选中强调色
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_radius(bar, 6, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kColorTabActive),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(bar, lv_color_hex(kColorText),
                                LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_t* content = lv_tabview_get_content(tv);
    screen_swipe_back_ignore(content, true);

    s_tab_overview = lv_tabview_add_tab(tv, I18n::T("今日"));
    StyleScrollTab(s_tab_overview);

    s_tab_forecast = lv_tabview_add_tab(tv, I18n::T("6日"));
    StyleScrollTab(s_tab_forecast);

    s_tab_hours = lv_tabview_add_tab(tv, I18n::T("24时"));
    StyleScrollTab(s_tab_hours);

    s_tab_index = lv_tabview_add_tab(tv, I18n::T("指数"));
    StyleScrollTab(s_tab_index);
}

void OnSwipeBack() {
    if (WeatherScreen::TryHandleNavBack()) {
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
    s_session.fetch_add(1, std::memory_order_relaxed);
    CloseCityPickerDialog();
    s_screen = nullptr;
    s_tabview = nullptr;
    s_tab_overview = nullptr;
    s_tab_forecast = nullptr;
    s_tab_hours = nullptr;
    s_tab_index = nullptr;
    s_status_lbl = nullptr;
}

}  // namespace

bool WeatherScreen::TryHandleNavBack() {
    if (s_city_dlg_mask == nullptr) {
        return false;
    }
    CloseCityPickerDialog();
    return true;
}

lv_obj_t* WeatherScreen::Create() {
    s_session.fetch_add(1, std::memory_order_relaxed);

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelW);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(kColorBgGrad), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    BuildTabView(scr);

    lv_obj_t* top = lv_obj_create(scr);
    lv_obj_set_size(top, kPanelW, kHeaderH);
    lv_obj_set_pos(top, 0, 0);
    screen_strip_obj_chrome(top);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(top, lv_color_hex(kColorHeaderBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t* divider = lv_obj_create(top);
    screen_strip_obj_chrome(divider);
    lv_obj_set_size(divider, kPanelW, 1);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kColorDivider), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    screen_make_input_passive(divider);

    lv_obj_t* title = MakeLabel(top, I18n::T("天气"), font_sub(), kColorText);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    s_status_lbl = MakeLabel(top, "", font_sub(), kColorSubtle);
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_MID, 0, 2);
    lv_obj_add_flag(s_status_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* refresh = lv_button_create(top);
    lv_obj_set_size(refresh, kRefreshBtnW, kHeaderBtnH);
    lv_obj_align(refresh, LV_ALIGN_RIGHT_MID, -kHeaderSidePad, 0);
    lv_obj_set_ext_click_area(refresh, panel::kTouchExt);
    StyleHeaderBtn(refresh);
    lv_obj_add_event_cb(refresh, OnRefreshClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* refresh_lbl = lv_label_create(refresh);
    lv_label_set_text(refresh_lbl, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_font(refresh_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(refresh_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_center(refresh_lbl);
    lv_obj_remove_flag(refresh_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* region = lv_button_create(top);
    lv_obj_set_size(region, kRegionBtnW, kHeaderBtnH);
    lv_obj_align(region, LV_ALIGN_RIGHT_MID,
                 -(kHeaderSidePad + kRefreshBtnW + kHeaderBtnGap), 0);
    lv_obj_set_ext_click_area(region, panel::kTouchExt);
    StyleHeaderBtn(region);
    lv_obj_add_event_cb(region, OnCityPickerOpenClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* region_lbl = lv_label_create(region);
    lv_label_set_text(region_lbl, LV_SYMBOL_GPS);
    lv_obj_set_style_text_font(region_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(region_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_center(region_lbl);
    lv_obj_remove_flag(region_lbl, LV_OBJ_FLAG_CLICKABLE);

    const std::string saved_id = LoadSavedDistrictId();
    WeatherService::Instance().SetDistrictId(saved_id);

    ShowLoadingPlaceholder();
    TriggerFetch();

    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    screen_attach_swipe_back(scr, OnSwipeBack);
    return scr;
}
