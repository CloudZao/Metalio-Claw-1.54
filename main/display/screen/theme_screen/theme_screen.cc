#include "theme_screen.h"
#include "i18n.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdint>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "home_screen/home_screen.h"
#include "panel_metrics.h"
#include "screen_util.h"
#include "theme_manager.h"

// lv_image_decoder_dsc_t 完整定义在私有头；公开 lvgl.h 只有前向声明。
#include "draw/lv_image_decoder_private.h"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);

namespace {

constexpr const char* TAG = "ThemeScreen";

constexpr int kPanelSize   = panel::kSize;
constexpr int kHeaderH     = panel::kHeaderH;

// 一页横排 2 个主题；左右滑切换页。
constexpr int kPerPage      = 2;
constexpr int kCardSize     = 88;
constexpr int kCardColGap   = 10;
constexpr int kCardRadius   = 14;
constexpr int kCardBorder   = 2;
constexpr int kCardLabelH   = 16;
constexpr int kCardLabelPad = 4;

// 底部分页圆点（与主屏视觉一致）
constexpr int kDotSize            = 5;
constexpr int kDotActiveW         = 12;
constexpr int kDotGap             = 5;
constexpr int kIndicatorYOffset   = 10;
constexpr int kIndicatorAreaH     = panel::kIndicatorH;
constexpr int kMaxThemePages      = 2;
constexpr uint32_t kDotColor      = 0xFFFFFF;

constexpr uint32_t kColorBg     = 0x000000;
constexpr uint32_t kColorText   = 0xFFFFFF;
constexpr uint32_t kColorActive = 0x3B82F6;

// lv_image_set_src 不拷贝字符串，路径必须是静态/长寿命缓冲。
constexpr int kPreviewPathBufSize = 40;
char s_preview_paths[ThemeManager::kThemeCount][kPreviewPathBufSize];

// ---------------------------------------------------------------------------
// 主题预览预光栅化
//
// SPNG 分片 + no_cache：滑动时每帧 get_area 重解码会裂帧。
// 创建时解到 PSRAM，之后 lv_image 走 VARIABLE 源，只做内存 blit。
// ---------------------------------------------------------------------------
struct ThemePreviewRaster {
    lv_image_dsc_t dsc{};
    uint8_t* pixels = nullptr;
    bool ready = false;
};

ThemePreviewRaster s_preview_rasters[ThemeManager::kThemeCount];

struct UiState {
    lv_obj_t* screen = nullptr;
    int current_theme = ThemeManager::kDefaultThemeId;
    lv_obj_t* indicator = nullptr;
    lv_obj_t* dots[kMaxThemePages] = {};
    lv_obj_t* cards[ThemeManager::kThemeCount] = {};
    lv_obj_t* labels[ThemeManager::kThemeCount] = {};
    int page_count = 0;
    int current_page = 0;
};
UiState s_ui;

void OnSwipeBack();
void GoHomeWithCurrentTheme();
void BuildHeader(lv_obj_t* parent);
void BuildContent(lv_obj_t* parent);
void HighlightDot(int page);
void CreateIndicator(lv_obj_t* screen, int page_count, int start_page);
void OnPagerScrollEnd(lv_event_t* e);
void EnsurePreviewPathsBuilt();
void FreeThemePreviewRasters();
void ApplyThemeSelection(int theme_id);

int ThemeIndex(int theme_id) {
    return theme_id - ThemeManager::kMinThemeId;
}

bool ThemeIdValid(int theme_id) {
    return theme_id >= ThemeManager::kMinThemeId &&
           theme_id <= ThemeManager::kMaxThemeId;
}

void FreeThemePreviewRasters() {
    for (int i = 0; i < ThemeManager::kThemeCount; ++i) {
        if (s_preview_rasters[i].pixels != nullptr) {
            heap_caps_free(s_preview_rasters[i].pixels);
        }
        s_preview_rasters[i] = ThemePreviewRaster{};
    }
}

bool CopyDecoderTileToRaster(uint8_t* dst, uint32_t dst_stride, uint32_t bpp,
                             const lv_draw_buf_t* tile, const lv_area_t& area) {
    if (dst == nullptr || tile == nullptr || tile->data == nullptr) {
        return false;
    }
    const int32_t tile_h = area.y2 - area.y1 + 1;
    const int32_t tile_w = area.x2 - area.x1 + 1;
    if (tile_h <= 0 || tile_w <= 0) {
        return false;
    }
    const uint32_t row_bytes = static_cast<uint32_t>(tile_w) * bpp;
    for (int32_t row = 0; row < tile_h; ++row) {
        const uint8_t* src =
            static_cast<const uint8_t*>(tile->data) +
            static_cast<uint32_t>(row) * tile->header.stride;
        uint8_t* out = dst + static_cast<uint32_t>(area.y1 + row) * dst_stride +
                       static_cast<uint32_t>(area.x1) * bpp;
        std::memcpy(out, src, row_bytes);
    }
    return true;
}

bool DecodeThemePreviewToRaster(int idx, const char* path) {
    if (idx < 0 || idx >= ThemeManager::kThemeCount || path == nullptr ||
        path[0] == '\0') {
        return false;
    }
    if (s_preview_rasters[idx].ready) {
        return true;
    }

    lv_image_header_t header = {};
    if (lv_image_decoder_get_info(path, &header) != LV_RESULT_OK ||
        header.w == 0 || header.h == 0) {
        ESP_LOGW(TAG, "preview info fail: %s", path);
        return false;
    }

    lv_image_decoder_dsc_t dec = {};
    if (lv_image_decoder_open(&dec, path, nullptr) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "preview open fail: %s", path);
        return false;
    }

    lv_color_format_t cf = static_cast<lv_color_format_t>(header.cf);
    if (dec.header.cf != 0) {
        cf = static_cast<lv_color_format_t>(dec.header.cf);
    }
    const uint32_t w = dec.header.w != 0 ? dec.header.w : header.w;
    const uint32_t h = dec.header.h != 0 ? dec.header.h : header.h;
    const uint32_t bpp = lv_color_format_get_size(cf);
    if (bpp == 0 || w == 0 || h == 0) {
        lv_image_decoder_close(&dec);
        ESP_LOGW(TAG, "preview bad format: %s cf=%u", path,
                 static_cast<unsigned>(cf));
        return false;
    }

    const uint32_t stride = lv_draw_buf_width_to_stride(w, cf);
    const size_t bytes = static_cast<size_t>(stride) * h;
    uint8_t* pixels = static_cast<uint8_t*>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr) {
        pixels = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    }
    if (pixels == nullptr) {
        lv_image_decoder_close(&dec);
        ESP_LOGE(TAG, "preview alloc %u bytes fail: %s",
                 static_cast<unsigned>(bytes), path);
        return false;
    }
    std::memset(pixels, 0, bytes);

    bool ok = false;
    if (dec.decoded != nullptr && dec.decoded->data != nullptr &&
        dec.decoded->header.w == static_cast<int32_t>(w) &&
        dec.decoded->header.h == static_cast<int32_t>(h)) {
        const lv_area_t full = {
            0, 0, static_cast<lv_coord_t>(w - 1), static_cast<lv_coord_t>(h - 1)};
        ok = CopyDecoderTileToRaster(pixels, stride, bpp, dec.decoded, full);
    } else {
        lv_area_t full = {0, 0, static_cast<lv_coord_t>(w - 1),
                          static_cast<lv_coord_t>(h - 1)};
        lv_area_t area = {LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN};
        ok = true;
        while (lv_image_decoder_get_area(&dec, &full, &area) == LV_RESULT_OK) {
            if (!CopyDecoderTileToRaster(pixels, stride, bpp, dec.decoded,
                                         area)) {
                ok = false;
                break;
            }
        }
    }
    lv_image_decoder_close(&dec);

    if (!ok) {
        heap_caps_free(pixels);
        ESP_LOGW(TAG, "preview decode fail: %s", path);
        return false;
    }

    ThemePreviewRaster& slot = s_preview_rasters[idx];
    slot.pixels = pixels;
    std::memset(&slot.dsc, 0, sizeof(slot.dsc));
    slot.dsc.data = pixels;
    slot.dsc.data_size = static_cast<uint32_t>(bytes);
    slot.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    slot.dsc.header.cf = cf;
    slot.dsc.header.w = static_cast<uint32_t>(w);
    slot.dsc.header.h = static_cast<uint32_t>(h);
    slot.dsc.header.stride = stride;
    slot.ready = true;
    ESP_LOGI(TAG, "preview rasterized [%d] %ux%u cf=%u %uB: %s", idx, w, h,
             static_cast<unsigned>(cf), static_cast<unsigned>(bytes), path);
    return true;
}

const lv_image_dsc_t* GetThemePreviewRaster(int theme_id) {
    if (!ThemeIdValid(theme_id)) {
        return nullptr;
    }
    const int idx = ThemeIndex(theme_id);
    if (!s_preview_rasters[idx].ready) {
        if (!DecodeThemePreviewToRaster(idx, s_preview_paths[idx])) {
            return nullptr;
        }
    }
    return &s_preview_rasters[idx].dsc;
}

void EnsurePreviewPathsBuilt() {
    for (int i = 0; i < ThemeManager::kThemeCount; ++i) {
        const int theme_id = ThemeManager::kMinThemeId + i;
        std::snprintf(s_preview_paths[i], kPreviewPathBufSize,
                      "A:ic_s_theme_preview_%d.spng", theme_id);
    }
}

void ApplyThemeSelection(int theme_id) {
    for (int i = 0; i < ThemeManager::kThemeCount; ++i) {
        const int id = ThemeManager::kMinThemeId + i;
        const bool active = (id == theme_id);
        lv_obj_t* card = s_ui.cards[i];
        lv_obj_t* lbl = s_ui.labels[i];
        if (card != nullptr) {
            lv_obj_set_style_border_opa(card, active ? LV_OPA_COVER : LV_OPA_TRANSP,
                                        LV_PART_MAIN);
        }
        if (lbl != nullptr) {
            char buf[32];
            if (active) {
                std::snprintf(buf, sizeof(buf), I18n::T("主题 %d  当前"), id);
                lv_obj_set_style_text_color(lbl, lv_color_hex(kColorActive),
                                            LV_PART_MAIN);
            } else {
                std::snprintf(buf, sizeof(buf), I18n::T("主题 %d"), id);
                lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText),
                                            LV_PART_MAIN);
            }
            lv_label_set_text(lbl, buf);
        }
    }
}

void OnCardClicked(lv_event_t* e) {
    const int target = static_cast<int>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (!ThemeIdValid(target)) {
        return;
    }
    if (target == s_ui.current_theme) {
        ESP_LOGI(TAG, "theme%d already active, skip", target);
        return;
    }
    ESP_LOGI(TAG, "switch theme -> theme%d (inplace)", target);
    ThemeManager::SetCurrentThemeId(target);
    s_ui.current_theme = target;
    ApplyThemeSelection(target);
}

lv_obj_t* BuildThemeCard(lv_obj_t* parent, int theme_id) {
    const bool active = (theme_id == s_ui.current_theme);
    const int idx = ThemeIndex(theme_id);

    lv_obj_t* slot = lv_obj_create(parent);
    lv_obj_remove_style_all(slot);
    lv_obj_set_size(slot, kCardSize, kCardSize + kCardLabelH + kCardLabelPad);
    lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(slot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* card = lv_obj_create(slot);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardSize, kCardSize);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(card, kCardRadius, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, kCardBorder, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(kColorActive), LV_PART_MAIN);
    lv_obj_set_style_border_opa(card, active ? LV_OPA_COVER : LV_OPA_TRANSP,
                                LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
    // 不对预览图 runtime clip，避免滚动时圆角 mask 加重裂帧。
    lv_obj_set_style_clip_corner(card, false, LV_PART_MAIN);

    lv_obj_t* preview = lv_image_create(card);
    if (const lv_image_dsc_t* raster = GetThemePreviewRaster(theme_id);
        raster != nullptr) {
        lv_image_set_src(preview, raster);
    } else {
        // 解码失败时回退路径字符串（仍可能卡，但保证有图可看）。
        lv_image_set_src(preview, s_preview_paths[idx]);
    }
    // 预览图已压成 84×84，与卡片内容区 1:1
    constexpr int kPreviewPx = kCardSize - kCardBorder * 2;
    lv_obj_set_size(preview, kPreviewPx, kPreviewPx);
    lv_obj_center(preview);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        card, OnCardClicked, LV_EVENT_CLICKED,
        reinterpret_cast<void*>(static_cast<intptr_t>(theme_id)));

    lv_obj_t* lbl = lv_label_create(slot);
    char buf[32];
    if (active) {
        std::snprintf(buf, sizeof(buf), I18n::T("主题 %d  当前"), theme_id);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorActive),
                                    LV_PART_MAIN);
    } else {
        std::snprintf(buf, sizeof(buf), I18n::T("主题 %d"), theme_id);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
    }
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_font(lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    if (idx >= 0 && idx < ThemeManager::kThemeCount) {
        s_ui.cards[idx] = card;
        s_ui.labels[idx] = lbl;
    }

    return slot;
}

void HighlightDot(int page) {
    if (page < 0 || page >= s_ui.page_count) {
        return;
    }
    s_ui.current_page = page;
    for (int i = 0; i < s_ui.page_count; ++i) {
        lv_obj_t* dot = s_ui.dots[i];
        if (dot == nullptr) {
            continue;
        }
        const bool on = (i == page);
        lv_obj_set_size(dot, on ? kDotActiveW : kDotSize, kDotSize);
        lv_obj_set_style_radius(dot, on ? 3 : LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, on ? LV_OPA_COVER : LV_OPA_30, LV_PART_MAIN);
    }
}

void CreateIndicator(lv_obj_t* screen, int page_count, int start_page) {
    if (page_count <= 1) {
        s_ui.indicator = nullptr;
        s_ui.page_count = page_count;
        s_ui.current_page = 0;
        for (int i = 0; i < kMaxThemePages; ++i) {
            s_ui.dots[i] = nullptr;
        }
        return;
    }

    const int count = page_count > kMaxThemePages ? kMaxThemePages : page_count;
    s_ui.page_count = count;

    lv_obj_t* indicator = lv_obj_create(screen);
    lv_obj_remove_style_all(indicator);
    lv_obj_set_size(indicator, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(indicator, LV_ALIGN_BOTTOM_MID, 0, -kIndicatorYOffset);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_column(indicator, kDotGap, LV_PART_MAIN);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicator, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
    s_ui.indicator = indicator;

    for (int i = 0; i < kMaxThemePages; ++i) {
        s_ui.dots[i] = nullptr;
    }
    for (int i = 0; i < count; ++i) {
        lv_obj_t* dot = lv_obj_create(indicator);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, kDotSize, kDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(kDotColor), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_30, LV_PART_MAIN);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        s_ui.dots[i] = dot;
    }

    HighlightDot(start_page);
}

void OnPagerScrollEnd(lv_event_t* e) {
    lv_obj_t* pager = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (pager == nullptr || s_ui.page_count <= 1) {
        return;
    }
    const int32_t scroll_x = lv_obj_get_scroll_x(pager);
    int page = static_cast<int>((scroll_x + kPanelSize / 2) / kPanelSize);
    if (page < 0) {
        page = 0;
    } else if (page >= s_ui.page_count) {
        page = s_ui.page_count - 1;
    }
    if (page != s_ui.current_page) {
        HighlightDot(page);
    }
}

void BuildHeader(lv_obj_t* parent) {
    lv_obj_t* title = lv_label_create(parent);
    lv_label_set_text(title, I18n::T("主题"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, (kHeaderH - 20) / 2);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);
}

void BuildContent(lv_obj_t* parent) {
    EnsurePreviewPathsBuilt();
    for (int i = 0; i < ThemeManager::kThemeCount; ++i) {
        s_ui.cards[i] = nullptr;
        s_ui.labels[i] = nullptr;
        // 进入页面时预解全部预览，避免首滑才解码。
        DecodeThemePreviewToRaster(i, s_preview_paths[i]);
    }

    constexpr int kCellH = kCardSize + kCardLabelH + kCardLabelPad;
    const int page_count =
        (ThemeManager::kThemeCount + kPerPage - 1) / kPerPage;
    const int pager_h = kPanelSize - kHeaderH;

    lv_obj_t* pager = lv_obj_create(parent);
    screen_strip_obj_chrome(pager);
    lv_obj_set_size(pager, kPanelSize, pager_h);
    lv_obj_set_pos(pager, 0, kHeaderH);
    lv_obj_set_style_bg_opa(pager, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pager, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(pager, LV_OBJ_FLAG_SCROLL_ONE);
    screen_swipe_back_ignore(pager, true);

    const int row_w = kPerPage * kCardSize + (kPerPage - 1) * kCardColGap;
    const int origin_x = (kPanelSize - row_w) / 2;
    // 预留下方指示区，避免卡片与分页点重叠
    const int origin_y = ((pager_h - kIndicatorAreaH) - kCellH) / 2;
    const int y = origin_y > 4 ? origin_y : 4;

    int start_page = 0;
    for (int page = 0; page < page_count; ++page) {
        lv_obj_t* page_obj = lv_obj_create(pager);
        screen_strip_obj_chrome(page_obj);
        lv_obj_set_size(page_obj, kPanelSize, pager_h);
        lv_obj_set_pos(page_obj, page * kPanelSize, 0);
        lv_obj_set_style_bg_opa(page_obj, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_remove_flag(page_obj, LV_OBJ_FLAG_SCROLLABLE);

        for (int col = 0; col < kPerPage; ++col) {
            const int idx = page * kPerPage + col;
            if (idx >= ThemeManager::kThemeCount) {
                break;
            }
            const int theme_id = ThemeManager::kMinThemeId + idx;
            if (theme_id == s_ui.current_theme) {
                start_page = page;
            }
            lv_obj_t* slot = BuildThemeCard(page_obj, theme_id);
            lv_obj_set_pos(slot, origin_x + col * (kCardSize + kCardColGap), y);
        }
    }

    if (start_page > 0) {
        lv_obj_scroll_to_x(pager, start_page * kPanelSize, LV_ANIM_OFF);
    }

    lv_obj_add_event_cb(pager, OnPagerScrollEnd, LV_EVENT_SCROLL_END, nullptr);
    CreateIndicator(parent, page_count, start_page);
}

void GoHomeWithCurrentTheme() {
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
    FreeThemePreviewRasters();
    s_ui = UiState{};
}

void OnSwipeBack() {
    GoHomeWithCurrentTheme();
}

}  // namespace

lv_obj_t* ThemeScreen::Create() {
    ESP_LOGI(TAG, "create theme screen");

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    s_ui.current_theme = ThemeManager::GetCurrentThemeId();

    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    BuildHeader(scr);
    BuildContent(scr);

    screen_attach_swipe_back(scr, OnSwipeBack);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    return scr;
}

void ThemeScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load");
    } else {
        ESP_LOGI(TAG, "unload");
    }
}
