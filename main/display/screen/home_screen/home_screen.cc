#include "home_screen.h"
#include "i18n.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <string>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <font_awesome.h>

#include "application.h"
#include "board.h"
#include "board_hw.h"
#include "dual_network_board.h"
#include "settings.h"
#include "settings_screen/settings_screen.h"
#include "calculator_screen/calculator_screen.h"
#include "chat_screen/chat_screen.h"
#include "game_2048_screen/game_2048_screen.h"
#include "ai_image_gen_screen/ai_image_gen_screen.h"
#include "translate_screen/translate_screen.h"
#include "pwr_key_handler.h"
#include "panel_metrics.h"
#include "screen_util.h"
#include "idle_power_policy.h"
#include "weather_screen/weather_screen.h"
#include "theme_manager.h"
#include "wifi_required_dialog.h"

// lv_image_decoder_dsc_t 完整定义在私有头；公开 lvgl.h 只有前向声明。
#include "draw/lv_image_decoder_private.h"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_awesome_20_4);

// 电池显示模式：1 = 图标（Font Awesome 字形，放最右边），0 = 文字（"电量 NN% 充电中 4.02V"）
#define HOME_STATUS_SHOW_BATTERY_ICON 1

namespace {

constexpr const char* TAG_HOME = "HomeScreen";

// ---------------------------------------------------------------------------
// Per-app lifecycle callbacks
//
// Each launcher hands its callback to screen_attach_lifecycle() so we get a
// LOAD notification right after the new screen becomes active, and an
// UNLOAD notification when LVGL switches away from it.  For now we only log
// the transitions -- but this is the right place to hang start / stop
// behaviour that should track a specific app's lifetime (e.g. pausing the
// audio player when the player screen is dismissed).
//
// All callbacks share the same shape so they can all sit in the AppEntry
// table below.  A nullptr callback simply skips logging for that app.
// ---------------------------------------------------------------------------

void game_2048_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("game_2048", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: game_2048");
    } else {
        ESP_LOGI(TAG_HOME, "unload: game_2048");
    }
}

void calculator_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("calculator", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: calculator");
    } else {
        ESP_LOGI(TAG_HOME, "unload: calculator");
    }
}

void weather_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("weather", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: weather_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: weather_screen");
    }
}

void chat_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("chat", event);
    ChatScreen::LifecycleCallback(event);
}

void ai_image_gen_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("ai_image_gen", event);
    AiImageGenScreen::LifecycleCallback(event);
}

void translate_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("translate", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: translate_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: translate_screen");
    }
    TranslateScreen::LifecycleCallback(event);
}

constexpr int kPanelSize = panel::kSize;
constexpr int kStatusBarHeight = panel::kStatusBarH;
constexpr int kIndicatorAreaHeight = panel::kIndicatorH;
constexpr int kPagerHeight =
    kPanelSize - kStatusBarHeight - kIndicatorAreaHeight;
constexpr int kAppsPerPage = 1;
constexpr int kIconSize = panel::kIconDrawPx;
constexpr int kCellWidth = 200;
constexpr int kNameGap = 6;
constexpr int kNameAreaH = 24;
constexpr int kCellHeight = kIconSize + kNameGap + kNameAreaH;
// 240 一页一卡：左右滑保留真实图标。卡顿主因是 SPNG 按条带每帧重解码，
// 用下方 PSRAM 预光栅化缓存解决，勿再靠深色骨架糊弄显示。
constexpr bool kPagerUseSkeletonWhileScrolling = false;

constexpr uint32_t kStatusBarBg = 0x000000;
constexpr int kMaxPages = 16;  // one app per page

// Indicator dot geometry
constexpr int kDotSize = 5;
constexpr int kDotActiveW = 12;
constexpr int kDotGap = 5;
constexpr int kIndicatorPadHor = 0;
constexpr int kIndicatorPadVer = 0;
constexpr int kIndicatorYOffset = 10;  // distance from panel bottom
constexpr uint32_t kIndicatorBg = 0x000000;
constexpr uint32_t kDotColor = 0xFFFFFF;

// Launchers take the lifecycle callback as an argument so we can attach it
// to the screen object before lv_screen_load() fires LV_EVENT_SCREEN_LOADED
// -- otherwise the first LOAD event would be missed.  Passing nullptr is
// supported (screen_attach_lifecycle no-ops in that case).
typedef void (*LaunchFn)(screen_lifecycle_cb_t lifecycle_cb);

struct AppEntry {
    // 图标资源后缀，例如 "chat"、"2048"。实际 LVGL 路径由 ThemeManager
    // 的当前主题 id 拼出：A:ic_app_home_theme{N}_{suffix}.spng。
    // 这样切换主题只需要重启后重新拼一次路径，不用改 AppEntry。
    const char* icon_suffix;
    const char* name;                    // display name shown under the icon
    LaunchFn launch;                     // tapped -> launch this app (nullptr = no action)
    screen_lifecycle_cb_t lifecycle_cb;  // load / unload observer
    // WiFi 模式下进入前必须已连上 WiFi；4G 模式不拦（走蜂窝）。
    bool requires_wifi;
};

void LaunchGame2048(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* game = Game2048::Create();
    screen_attach_lifecycle(game, lifecycle_cb);
    lv_screen_load(game);
    if (old_scr != nullptr && old_scr != game) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchCalculator(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = Calculator::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchWeather(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = WeatherScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchChat(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = ChatScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}



void LaunchAiImageGen(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = AiImageGenScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchTranslate(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = TranslateScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void LaunchSettings(screen_lifecycle_cb_t lifecycle_cb) {
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* app = SettingsScreen::Create();
    screen_attach_lifecycle(app, lifecycle_cb);
    lv_screen_load(app);
    if (old_scr != nullptr && old_scr != app) {
        lv_obj_delete_async(old_scr);
    }
}

void settings_lifecycle_cb(screen_lifecycle_event_t event) {
    PwrKey_OnScreenLifecycle("settings", event);
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG_HOME, "load: settings_screen");
    } else {
        ESP_LOGI(TAG_HOME, "unload: settings_screen");
    }
    SettingsScreen::LifecycleCallback(event);
}

// 主屏 app 表。icon_suffix 对应 ic_app_home_theme{N}_{suffix}.spng 的
// 中间 suffix 部分；name 显示在图标正下方（传统手机桌面样式）。
// 注意 "定位" 的 suffix 是 "gps"（资源命名），不是 "map"；"磁场" 暂无
// 主题图标，资源 ic_app_home_themeN_magnet.spng 缺失时该格会显示为空。
// name 存 zh-CN msgid（源文案）；显示时用 I18n::T(entry.name)。
// 星智 1.54 Metal 主屏 App 顺序：
// 聊天 → AI绘图 → 翻译 → 天气 → 计算器 → 2048 → 设置
// 网络/主题/OTA/系统信息等改从设置进入。
constexpr AppEntry kApps[] = {
    {"chat",           "聊天",     LaunchChat,          chat_lifecycle_cb,          true},
    {"ai_image_gen",   "AI绘图",   LaunchAiImageGen,    ai_image_gen_lifecycle_cb,  true},
    {"translate",      "翻译",     LaunchTranslate,     translate_lifecycle_cb,     true},
    {"weather",        "天气",     LaunchWeather,       weather_lifecycle_cb,       true},
    {"calculator",     "计算器",   LaunchCalculator,    calculator_lifecycle_cb,    false},
    {"2048",           "2048",     LaunchGame2048,      game_2048_lifecycle_cb,     false},
    {"settings",       "设置",     LaunchSettings,      settings_lifecycle_cb,      false},
};

constexpr int kTotalApps = static_cast<int>(sizeof(kApps) / sizeof(kApps[0]));

// 当前主题下，每个 app 解析出来的完整 LVGL 路径。在 HomeScreen::Create()
// 入口处由 EnsureIconPathsBuilt() 填写；主题 id 变化时会自动重新拼接。
//
// 路径字符串必须在 lv_image 引用期间一直有效（lv_image_set_src 不复制内
// 存），所以放在 namespace 静态而不是栈或函数局部。
constexpr int kIconPathBufSize = 56;
char s_icon_paths[kTotalApps][kIconPathBufSize];

// ---------------------------------------------------------------------------
// 主屏图标预光栅化
//
// SPNG 是分片格式：滑动时 LVGL 每帧走 get_area 重新解压，SPI 刷屏叠解码
// 就会卡。创建时一次性解到 PSRAM 位图，之后 lv_image 走 VARIABLE 源，
// 滑动只做内存 blit。克隆页共用同一份 dsc。主题切换时整表重建。
// ---------------------------------------------------------------------------
struct HomeIconRaster {
    lv_image_dsc_t dsc{};
    uint8_t* pixels = nullptr;
    bool ready = false;
};

HomeIconRaster s_icon_rasters[kTotalApps];

void FreeHomeIconRasters() {
    for (int i = 0; i < kTotalApps; ++i) {
        if (s_icon_rasters[i].pixels != nullptr) {
            heap_caps_free(s_icon_rasters[i].pixels);
        }
        s_icon_rasters[i] = HomeIconRaster{};
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

bool DecodeHomeIconToRaster(int idx, const char* path) {
    if (idx < 0 || idx >= kTotalApps || path == nullptr || path[0] == '\0') {
        return false;
    }
    if (s_icon_rasters[idx].ready) {
        return true;
    }

    lv_image_header_t header = {};
    if (lv_image_decoder_get_info(path, &header) != LV_RESULT_OK ||
        header.w == 0 || header.h == 0) {
        ESP_LOGW(TAG_HOME, "icon info fail: %s", path);
        return false;
    }

    lv_image_decoder_dsc_t dec = {};
    if (lv_image_decoder_open(&dec, path, nullptr) != LV_RESULT_OK) {
        ESP_LOGW(TAG_HOME, "icon open fail: %s", path);
        return false;
    }

    // header.cf 是位域，读出来是整数，需显式转到枚举。
    lv_color_format_t cf = static_cast<lv_color_format_t>(header.cf);
    if (dec.header.cf != 0) {
        cf = static_cast<lv_color_format_t>(dec.header.cf);
    }
    const uint32_t w = dec.header.w != 0 ? dec.header.w : header.w;
    const uint32_t h = dec.header.h != 0 ? dec.header.h : header.h;
    const uint32_t bpp = lv_color_format_get_size(cf);
    if (bpp == 0 || w == 0 || h == 0) {
        lv_image_decoder_close(&dec);
        ESP_LOGW(TAG_HOME, "icon bad format: %s cf=%u", path,
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
        ESP_LOGE(TAG_HOME, "icon alloc %u bytes fail: %s",
                 static_cast<unsigned>(bytes), path);
        return false;
    }
    std::memset(pixels, 0, bytes);

    bool ok = false;
    if (dec.decoded != nullptr && dec.decoded->data != nullptr &&
        dec.decoded->header.w == static_cast<int32_t>(w) &&
        dec.decoded->header.h == static_cast<int32_t>(h)) {
        // 整图一次给出（非分片格式）。
        const lv_area_t full = {
            0, 0, static_cast<lv_coord_t>(w - 1), static_cast<lv_coord_t>(h - 1)};
        ok = CopyDecoderTileToRaster(pixels, stride, bpp, dec.decoded, full);
    } else {
        // SPNG 分片：循环 get_area 拼满。
        lv_area_t full = {0, 0, static_cast<lv_coord_t>(w - 1),
                          static_cast<lv_coord_t>(h - 1)};
        lv_area_t area = {LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN};
        ok = true;
        while (lv_image_decoder_get_area(&dec, &full, &area) == LV_RESULT_OK) {
            if (!CopyDecoderTileToRaster(pixels, stride, bpp, dec.decoded, area)) {
                ok = false;
                break;
            }
        }
    }
    lv_image_decoder_close(&dec);

    if (!ok) {
        heap_caps_free(pixels);
        ESP_LOGW(TAG_HOME, "icon decode fail: %s", path);
        return false;
    }

    HomeIconRaster& slot = s_icon_rasters[idx];
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
    ESP_LOGI(TAG_HOME, "icon rasterized [%d] %ux%u cf=%u %uB: %s", idx, w, h,
             static_cast<unsigned>(cf), static_cast<unsigned>(bytes), path);
    return true;
}

const lv_image_dsc_t* GetHomeIconRaster(int idx) {
    if (idx < 0 || idx >= kTotalApps) {
        return nullptr;
    }
    if (!s_icon_rasters[idx].ready) {
        if (!DecodeHomeIconToRaster(idx, s_icon_paths[idx])) {
            return nullptr;
        }
    }
    return &s_icon_rasters[idx].dsc;
}

void EnsureIconPathsBuilt() {
    static bool built = false;
    static int s_built_theme_id = 0;
    const int tid = ThemeManager::GetCurrentThemeId();
    // 第一次构建 / 主题 id 变更（理论上只在调试热改 NVS 时出现），都重新拼。
    if (built && s_built_theme_id == tid) {
        return;
    }
    FreeHomeIconRasters();
    for (int i = 0; i < kTotalApps; ++i) {
        std::snprintf(s_icon_paths[i], kIconPathBufSize,
                      "A:ic_app_home_theme%d_%s.spng",
                      tid, kApps[i].icon_suffix);
    }
    s_built_theme_id = tid;
    built = true;
    ESP_LOGI(TAG_HOME, "icon paths built for theme%d", tid);
}

// ---------------------------------------------------------------------------
// 主屏触摸：一次「无→有→无」为一段会话，screen 级统一分类与分发。
// 不再使用 cell 的 LV_EVENT_CLICKED。
// ---------------------------------------------------------------------------

constexpr int kHomeMoveThreshold = 5;       // 滑动激活 / 点击位移上限（|dx|、|dy| 均 < 此值才算 tap）
constexpr int kHomeAxisLockThreshold = 12;  // 消抖：主方向位移超过此值才锁定手势
constexpr int kPageSnapThreshold = kPanelSize / 5;  // 跟手拖动超过 20% 页宽即切页（720→144px）
constexpr int kHomeFlickThreshold = 24;       // 松手快速 fling 位移阈值
constexpr uint32_t kHomeLongPressMs = 750;  // 按下→松开 ≥ 此值且位移够小 → 长按
constexpr uint32_t kPageSlideAnimMs = 300;  // 左右翻页吸附动画时长

constexpr lv_obj_flag_t kAppCellFlag = LV_OBJ_FLAG_USER_2;

enum class HomeTouchKind {
    None,
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
    Click,
    LongPress,
};

enum class HomeGestureAxis {
    None,
    Horizontal,
    Vertical,
};

struct HomeTouchSession {
    bool active = false;
    bool consumed = false;  // 已由滑动手势消费
    bool paging = false;      // 水平跟手拖动翻页中
    HomeGestureAxis axis = HomeGestureAxis::None;
    int16_t start_x = 0;
    int16_t start_y = 0;
    int16_t last_x = 0;       // 跟手拖动时的上一采样点
    uint32_t press_tick = 0;
    lv_obj_t* press_cell = nullptr;
    const AppEntry* app = nullptr;
};
HomeTouchSession s_home_touch;

// ---------------------------------------------------------------------------
// 主屏无操作计时：由 idle_power_policy 统一管理（进入待机 + 累计关机）。
// ---------------------------------------------------------------------------
void BeginSystemShutdown(const char* reason);

void ResetHomeIdleTimer() { IdlePower_NotifyActivity(); }

void StopHomeIdleTimer() { IdlePower_Detach(IdlePowerSession::Home); }

void StartHomeIdleTimer() {
    IdlePower_Attach(IdlePowerSession::Home, /*reset_activity=*/true);
}

// App 卡片按压过渡：确认点击/长按后触发缩放。
constexpr uint32_t kCellPressScaleMs = 200;  // 与 GetPressTransition 时长一致

const lv_style_prop_t kPressTransProps[] = {
    LV_STYLE_TRANSFORM_SCALE_X,
    LV_STYLE_TRANSFORM_SCALE_Y,
    LV_STYLE_PROP_INV,
};

lv_style_transition_dsc_t& GetPressTransition() {
    static lv_style_transition_dsc_t dsc;
    static bool inited = false;
    if (!inited) {
        lv_style_transition_dsc_init(&dsc, kPressTransProps, lv_anim_path_ease_out,
                                     kCellPressScaleMs, 0, nullptr);
        inited = true;
    }
    return dsc;
}

// 返回 false：被 WiFi 未连接拦截（调用方需恢复图标按压态）。
bool LaunchHomeApp(const AppEntry* app) {
    if (app == nullptr || app->launch == nullptr) {
        return false;
    }
    // 仅对 requires_wifi 的应用做入口拦截；录音等本地可用的应用不在此拦。
    if (app->requires_wifi && WifiRequired_ShouldBlock()) {
        ESP_LOGW(TAG_HOME, "block app '%s': WiFi not connected",
                 app->icon_suffix != nullptr ? app->icon_suffix : "?");
        WifiRequired_ShowDialog();
        return false;
    }
    app->launch(app->lifecycle_cb);
    return true;
}

struct CellScaleCtx {
    lv_obj_t* cell = nullptr;
    const AppEntry* launch_app = nullptr;  // 非空：缩放结束后进入 app
};

lv_timer_t* s_cell_scale_timer = nullptr;
CellScaleCtx s_cell_scale_ctx;

void CancelCellScaleTimer() {
    if (s_cell_scale_timer == nullptr) {
        return;
    }
    lv_timer_delete(s_cell_scale_timer);
    s_cell_scale_timer = nullptr;
    s_cell_scale_ctx = CellScaleCtx{};
}

void OnCellScaleTimer(lv_timer_t* timer) {
    const CellScaleCtx ctx = s_cell_scale_ctx;
    s_cell_scale_timer = nullptr;
    s_cell_scale_ctx = CellScaleCtx{};
    lv_timer_delete(timer);

    if (ctx.launch_app != nullptr) {
        if (!LaunchHomeApp(ctx.launch_app) && ctx.cell != nullptr) {
            lv_obj_remove_state(ctx.cell, LV_STATE_PRESSED);
        }
        return;
    }
    if (ctx.cell != nullptr) {
        lv_obj_remove_state(ctx.cell, LV_STATE_PRESSED);
    }
}

// launch_app：单击传入 app，缩放 kCellPressScaleMs 后再 launch；长按传 nullptr，仅缩放回弹。
void PlayAppCellPressScale(lv_obj_t* cell, const AppEntry* launch_app) {
    if (cell == nullptr) {
        return;
    }
    CancelCellScaleTimer();
    lv_obj_add_state(cell, LV_STATE_PRESSED);
    s_cell_scale_ctx.cell = cell;
    s_cell_scale_ctx.launch_app = launch_app;
    s_cell_scale_timer =
        lv_timer_create(OnCellScaleTimer, kCellPressScaleMs, nullptr);
    lv_timer_set_repeat_count(s_cell_scale_timer, 1);
}

lv_obj_t* FindAppCellFromTarget(lv_obj_t* target, lv_obj_t* screen) {
    for (lv_obj_t* obj = target; obj != nullptr && obj != screen;
         obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, kAppCellFlag)) {
            return obj;
        }
    }
    return nullptr;
}

lv_obj_t* CreateAppCellSkeleton(lv_obj_t* cell) {
    // 翻页骨架：不透明直角。PPA 开启时半透明/圆角会先 msync 再软件 fallback，
    // 易刷 invalid addr；静止态图标仍保留圆角。尺寸与图标一致；骨架态隐藏名称。
    constexpr uint32_t kSkeletonBg = 0x2A2F3A;

    lv_obj_t* skeleton = lv_obj_create(cell);
    lv_obj_remove_style_all(skeleton);
    lv_obj_set_size(skeleton, kIconSize, kIconSize);
    lv_obj_set_style_bg_color(skeleton, lv_color_hex(kSkeletonBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(skeleton, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(skeleton, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(skeleton, 0, LV_PART_MAIN);
    lv_obj_align(skeleton, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(skeleton, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(skeleton, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(skeleton, LV_OBJ_FLAG_HIDDEN);
    return skeleton;
}

lv_obj_t* CreateAppCell(lv_obj_t* parent, const AppEntry& entry, int idx) {
    lv_obj_t* cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, kCellWidth, kCellHeight);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);

    // 单元格：上图标、下名称；不对整格 clip，避免裁切底部文字。
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_radius(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(cell, false, LV_PART_MAIN);
    lv_obj_set_style_transition(cell, &GetPressTransition(), LV_PART_MAIN);

    // 缩放绕图标中心；仅在 screen 确认 Click / LongPress 后 add_state(PRESSED)。
    lv_obj_set_style_transform_pivot_x(cell, kCellWidth / 2,
                                       LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transform_pivot_y(cell, kIconSize / 2,
                                       LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(cell, 262, LV_PART_MAIN | LV_STATE_PRESSED);

    // 优先用预光栅化位图（滑动流畅）；失败再回退 SPNG 路径。
    lv_obj_t* icon = lv_image_create(cell);
    if (const lv_image_dsc_t* raster = GetHomeIconRaster(idx);
        raster != nullptr) {
        lv_image_set_src(icon, raster);
    } else {
        lv_image_set_src(icon, s_icon_paths[idx]);
    }
    lv_obj_set_size(icon, kIconSize, kIconSize);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    // child[1]：翻页骨架（与 icon 同位同尺寸），见 SetPagerSkeletonMode。
    CreateAppCellSkeleton(cell);

    lv_obj_t* name = lv_label_create(cell);
    lv_label_set_text(name, entry.name != nullptr ? I18n::T(entry.name) : "");
    lv_obj_set_width(name, kCellWidth);
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(name, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, kIconSize + kNameGap);
    lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);

    if (entry.launch != nullptr) {
        // 可命中以便 PRESSED 时锁定 app；不用 LV_EVENT_CLICKED，由 screen 分发。
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(cell, kAppCellFlag);
        lv_obj_set_user_data(cell, const_cast<AppEntry*>(&entry));
    }
    return cell;
}

// ---------------------------------------------------------------------------
// Pager + page-indicator
//
// Layout
//   pager (720 x kPagerHeight) -- horizontal scroll, snap to page center.
//     each child is a Page object (also 720 x kPagerHeight) which holds a
//     3x3 grid of cells.  We mark every page LV_OBJ_FLAG_SNAPPABLE so a
//     swipe ends with one page perfectly centered.
//
// State
//   PagerState lives on the heap and is owned by the screen via
//   LV_EVENT_DELETE.  The scroll callback uses it to map scroll position
//   -> current page and re-tint the dots.  No globals; if HomeScreen is
//   torn down and rebuilt, the new instance allocates a fresh state.
// ---------------------------------------------------------------------------

struct PagerState {
    lv_obj_t* pager;
    lv_obj_t* indicator = nullptr;  // 底部页码胶囊；翻页中会临时隐藏
    lv_obj_t* dots[kMaxPages];
    int page_count;
    int current_page;
    bool skeleton_active = false;
};

// 记住用户上次停留在主屏的哪一页，便于从子页面返回时回到同一页。
// 子页面调用 HomeScreen::Create() 重建时，会读取这个值滚动到对应页。
// 用户每次翻页（GoToPage）会写入；HighlightDot 不写入，否则
// CreateIndicator 初始化时调用 HighlightDot(state, 0) 会把它清零。
int s_last_home_page = 0;
lv_obj_t* s_home_scr = nullptr;
PagerState* s_active_pager = nullptr;

struct HomeStatusState {
    lv_obj_t* bar = nullptr;
    lv_obj_t* network_icon_lbl = nullptr;
    lv_obj_t* network_type_lbl = nullptr;
    lv_obj_t* sim_slot_lbl = nullptr;       // 仅 4G 模式下显示：外置卡 / 内置卡
    lv_obj_t* battery_icon_lbl = nullptr;   // 电池图标（Font Awesome 字形）
    lv_obj_t* battery_pct_lbl  = nullptr;   // 电池电量文字，例如 I18n::T("电量 85%")
    lv_obj_t* time_lbl = nullptr;
    lv_timer_t* update_timer = nullptr;
    const char* last_icon = nullptr;
    const char* last_battery_icon = nullptr; // 缓存上次图标，避免重复 set_text
    int  last_battery_pct = -1;
    bool last_battery_low = false;  // 缓存上一次的低电量染色状态
    int  last_net_type = -1;        // 缓存上次显示的网络类型，避免每秒重绘 SIM 标签
    int  last_sim_slot = -1;        // 缓存上次显示的 SIM 槽位
};

HomeStatusState* s_home_status = nullptr;

void SetPagerSkeletonMode(PagerState* state, bool active) {
    if (state == nullptr || state->pager == nullptr) {
        return;
    }
    // 关闭骨架策略时强制保持图标可见（并把可能残留的骨架态清掉）。
    if (!kPagerUseSkeletonWhileScrolling) {
        active = false;
    }
    if (state->skeleton_active == active) {
        return;
    }
    state->skeleton_active = active;

    // PPA 对半透明 fill 会先 msync 再软件绘制；翻页重绘风暴里极易踩到
    // 非 cacheable 中间层。翻页期间把半透明装饰关掉/改不透明。
    if (state->indicator != nullptr) {
        if (active) {
            lv_obj_add_flag(state->indicator, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(state->indicator, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_home_status != nullptr && s_home_status->bar != nullptr) {
        lv_obj_set_style_bg_opa(s_home_status->bar,
                                active ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
    }

    const uint32_t page_child_count = lv_obj_get_child_count(state->pager);
    for (uint32_t p = 0; p < page_child_count; ++p) {
        lv_obj_t* page = lv_obj_get_child(state->pager, p);
        if (page == nullptr) {
            continue;
        }
        const uint32_t cell_count = lv_obj_get_child_count(page);
        for (uint32_t c = 0; c < cell_count; ++c) {
            lv_obj_t* cell = lv_obj_get_child(page, c);
            if (cell == nullptr || !lv_obj_has_flag(cell, kAppCellFlag)) {
                continue;
            }
            // child[0]=icon, [1]=skeleton, [2]=name
            lv_obj_t* icon = lv_obj_get_child(cell, 0);
            lv_obj_t* skeleton = lv_obj_get_child(cell, 1);
            lv_obj_t* name = lv_obj_get_child(cell, 2);
            if (icon == nullptr || skeleton == nullptr) {
                continue;
            }
            if (active) {
                lv_obj_remove_state(cell, LV_STATE_PRESSED);
                lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(skeleton, LV_OBJ_FLAG_HIDDEN);
                if (name != nullptr) {
                    lv_obj_add_flag(name, LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                lv_obj_add_flag(skeleton, LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
                if (name != nullptr) {
                    lv_obj_remove_flag(name, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }
}

void UpdateHomeStatusBar(HomeStatusState* st);

int GetSavedNetworkType() {
    // 优先用板级已加载的内存态，避免在 LVGL 任务里读 NVS（S3 PSRAM 栈会炸）。
    auto* dual = dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
    if (dual != nullptr) {
        return dual->GetNetworkType() == NetworkType::ML307 ? 1 : 0;
    }
    constexpr int kDefaultNetType = 1;  // 默认 4G
    const NetworkType type =
        DualNetworkBoard::LoadNetworkTypeFromSettings(kDefaultNetType);
    return type == NetworkType::ML307 ? 1 : 0;
}

// 与 network_screen 共用 "network/sim_slot" 这一 NVS key。
// 返回 0 = 外置卡（默认）/ 1 = 内置卡。
int GetSavedSimSlot() {
    Settings settings("network", true);
    int v = settings.GetInt("sim_slot", 0);
    return (v == 1) ? 1 : 0;
}

void SaveSimSlot(int slot) {
    Settings settings("network", true);
    settings.SetInt("sim_slot", slot);
}


void UpdateHomeStatusBar(HomeStatusState* st) {
    if (st == nullptr || st->bar == nullptr) {
        return;
    }

    const int net_type = GetSavedNetworkType();
    if (st->network_type_lbl != nullptr) {
        lv_label_set_text(st->network_type_lbl, net_type == 1 ? "4G" : "WiFi");
    }

    // 1.54 无内外双 SIM，状态栏不显示卡槽。
    if (st->sim_slot_lbl != nullptr) {
        lv_obj_add_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    st->last_net_type = net_type;

    const char* icon = Board::GetInstance().GetNetworkStateIcon();
    if (icon != nullptr && st->network_icon_lbl != nullptr &&
        icon != st->last_icon) {
        st->last_icon = icon;
        lv_label_set_text(st->network_icon_lbl, icon);
    }

    // ---- 电池 ----
#if HOME_STATUS_SHOW_BATTERY_ICON
    // 图标模式：仅 Font Awesome 电池图标，靠最右边
    if (st->battery_icon_lbl != nullptr) {
        int battery_level = 0;
        bool charging = false, discharging = false;
        if (Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging)) {
            if (battery_level < 0)   battery_level = 0;
            if (battery_level > 100) battery_level = 100;

            const char* bat_icon = nullptr;
            if (charging) {
                bat_icon = FONT_AWESOME_BATTERY_BOLT;
            } else if (battery_level >= 80) {
                bat_icon = FONT_AWESOME_BATTERY_FULL;
            } else if (battery_level >= 60) {
                bat_icon = FONT_AWESOME_BATTERY_THREE_QUARTERS;
            } else if (battery_level >= 40) {
                bat_icon = FONT_AWESOME_BATTERY_HALF;
            } else if (battery_level >= 20) {
                bat_icon = FONT_AWESOME_BATTERY_QUARTER;
            } else {
                bat_icon = FONT_AWESOME_BATTERY_EMPTY;
            }
            if (bat_icon != st->last_battery_icon) {
                st->last_battery_icon = bat_icon;
                lv_label_set_text(st->battery_icon_lbl, bat_icon);
            }

            const bool low = !charging && battery_level < 20;
            if (low != st->last_battery_low) {
                st->last_battery_low = low;
                uint32_t color = low ? 0xF87171 : 0xFFFFFF;
                lv_obj_set_style_text_color(st->battery_icon_lbl,
                                            lv_color_hex(color), LV_PART_MAIN);
            }
        } else {
            // 无电池：斜杠图标，恢复白色
            st->last_battery_icon = FONT_AWESOME_BATTERY_SLASH;
            lv_label_set_text(st->battery_icon_lbl, FONT_AWESOME_BATTERY_SLASH);
            if (st->last_battery_low) {
                st->last_battery_low = false;
                lv_obj_set_style_text_color(st->battery_icon_lbl,
                                            lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            }
        }
    }
#else
    // 文字模式：电量百分比 + 充电中 + 电压（原有行为）
    if (st->battery_pct_lbl != nullptr) {
        int battery_level = 0;
        bool charging = false, discharging = false;
        bool has_battery = Board::GetInstance().GetBatteryLevel(
            battery_level, charging, discharging);

        if (has_battery) {
            if (battery_level < 0)   battery_level = 0;
            if (battery_level > 100) battery_level = 100;

            const bool low = !charging && battery_level < 20;

            char buf[48];
            const char* volt_str = "--V";
            if (charging) {
                std::snprintf(buf, sizeof(buf), I18n::T("电量 %d%% 充电中 %s"),
                              battery_level, volt_str);
            } else {
                std::snprintf(buf, sizeof(buf), I18n::T("电量 %d%% %s"),
                              battery_level, volt_str);
            }
            lv_label_set_text(st->battery_pct_lbl, buf);
            st->last_battery_pct = battery_level;

            if (low != st->last_battery_low) {
                st->last_battery_low = low;
                uint32_t color = low ? 0xF87171 : 0xFFFFFF;
                lv_obj_set_style_text_color(st->battery_pct_lbl,
                                            lv_color_hex(color), LV_PART_MAIN);
            }
        } else {
            if (st->last_battery_pct != -1) {
                st->last_battery_pct = -1;
                lv_label_set_text(st->battery_pct_lbl, I18n::T("电量 --%"));
            }
            if (st->last_battery_low) {
                st->last_battery_low = false;
                lv_obj_set_style_text_color(st->battery_pct_lbl,
                                            lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            }
        }
    }
#endif

    if (st->time_lbl != nullptr) {
        time_t now = time(nullptr);
        struct tm tm_info = {};
        if (localtime_r(&now, &tm_info) != nullptr &&
            tm_info.tm_year >= 2025 - 1900) {
            char time_str[16];
            strftime(time_str, sizeof(time_str), "%H:%M", &tm_info);
            lv_label_set_text(st->time_lbl, time_str);
        } else {
            lv_label_set_text(st->time_lbl, "--:--");
        }
    }
}

void OnHomeStatusTimer(lv_timer_t* timer) {
    UpdateHomeStatusBar(static_cast<HomeStatusState*>(lv_timer_get_user_data(timer)));
}

void OnHomeStatusDeleted(lv_event_t* e) {
    auto* st = static_cast<HomeStatusState*>(lv_event_get_user_data(e));
    if (st == nullptr) {
        return;
    }
    if (s_home_status == st) {
        s_home_status = nullptr;
    }
    if (st->update_timer != nullptr) {
        lv_timer_delete(st->update_timer);
        st->update_timer = nullptr;
    }
    delete st;
}

lv_obj_t* CreateStatusBar(lv_obj_t* screen, HomeStatusState* st) {
    // 左右两块容器宽度直接锁死，避免 LVGL flex 在 SIZE_CONTENT + SPACE_BETWEEN
    // 组合下对内容宽度的二次评估把右边「电量 100% 充电中」末尾几个字裁掉。
    //   - 左侧 300px：足够放下「图标 + 4G + 外置卡」
    //   - 右侧 400px：「电量 100% 充电中 HH:MM」是 ~18 字宽，预留充分
    //   - 中间空隙由父 flex SPACE_BETWEEN 自动撑开
    constexpr int kStatusLeftWidth  = 88;
    constexpr int kStatusRightWidth = 36;

    lv_obj_t* bar = lv_obj_create(screen);
    st->bar = bar;
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, kPanelSize, kStatusBarHeight);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kStatusBarBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(bar, 2, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* left = lv_obj_create(bar);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, kStatusLeftWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(left, 4, LV_PART_MAIN);

    st->network_icon_lbl = lv_label_create(left);
    lv_label_set_text(st->network_icon_lbl, FONT_AWESOME_WIFI);
    lv_obj_set_style_text_font(st->network_icon_lbl, &font_awesome_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->network_icon_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);

    st->network_type_lbl = lv_label_create(left);
    lv_label_set_long_mode(st->network_type_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->network_type_lbl, LV_SIZE_CONTENT);
    lv_label_set_text(st->network_type_lbl, "WiFi");
    lv_obj_set_style_text_font(st->network_type_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->network_type_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);

    // SIM 卡标签（外置卡 / 内置卡），用稍浅一些的灰色与「4G」做视觉区分。
    // 默认隐藏，UpdateHomeStatusBar 会根据当前网络类型决定显隐。
    st->sim_slot_lbl = lv_label_create(left);
    lv_label_set_long_mode(st->sim_slot_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->sim_slot_lbl, LV_SIZE_CONTENT);
    lv_label_set_text(st->sim_slot_lbl, "");
    lv_obj_set_style_text_font(st->sim_slot_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->sim_slot_lbl, lv_color_hex(0xC9D1D9),
                                LV_PART_MAIN);
    lv_obj_add_flag(st->sim_slot_lbl, LV_OBJ_FLAG_HIDDEN);

    // ---- 状态栏右侧：电池电量文字（时间单独居中浮在状态栏正中） ----
    // 故意不再放 font_awesome 电池图标，避免字符渲染不到 / 占位问题。
    // 右容器宽度固定 400px，靠右排电量文字，这样无论文本变长
    // （"电量 100% 充电中"）还是变短（"电量 --%"）都不会被父级 flex
    // 重排时挤压裁切。
    lv_obj_t* right = lv_obj_create(bar);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, kStatusRightWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
#if HOME_STATUS_SHOW_BATTERY_ICON
    // 图标模式：只放一个 Font Awesome 电池图标，靠最右边
    st->battery_icon_lbl = lv_label_create(right);
    lv_label_set_text(st->battery_icon_lbl, FONT_AWESOME_BATTERY_FULL);
    lv_obj_set_style_text_font(st->battery_icon_lbl, &font_awesome_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->battery_icon_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
#else
    // 文字模式：电量百分比 + 充电中 + 电压（原有行为）
    lv_obj_set_style_pad_column(right, 14, LV_PART_MAIN);

    st->battery_pct_lbl = lv_label_create(right);
    lv_label_set_long_mode(st->battery_pct_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->battery_pct_lbl, 380);
    lv_label_set_text(st->battery_pct_lbl, I18n::T("电量 --%"));
    lv_obj_set_style_text_align(st->battery_pct_lbl, LV_TEXT_ALIGN_RIGHT,
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(st->battery_pct_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->battery_pct_lbl, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
#endif

    // 时间居中：FLOATING 容器脱离 bar 的 flex 布局，锚在状态栏正中央。
    lv_obj_t* center = lv_obj_create(bar);
    lv_obj_add_flag(center, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_style_all(center);
    lv_obj_set_size(center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(center, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(center, LV_ALIGN_CENTER, 0, 0);

    st->time_lbl = lv_label_create(center);
    lv_label_set_long_mode(st->time_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(st->time_lbl, 64);
    lv_label_set_text(st->time_lbl, "--:--");
    lv_obj_set_style_text_align(st->time_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(st->time_lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(st->time_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    UpdateHomeStatusBar(st);
    st->update_timer = lv_timer_create(OnHomeStatusTimer, 1000, st);
    s_home_status = st;

    lv_obj_add_event_cb(screen, OnHomeStatusDeleted, LV_EVENT_DELETE, st);
    return bar;
}

void HighlightDot(PagerState* state, int page) {
    if (page < 0 || page >= state->page_count) {
        return;
    }
    state->current_page = page;
    for (int i = 0; i < state->page_count; ++i) {
        lv_obj_t* dot = state->dots[i];
        if (dot == nullptr) {
            continue;
        }
        const bool on = (i == page);
        lv_obj_set_size(dot, on ? kDotActiveW : kDotSize, kDotSize);
        lv_obj_set_style_radius(dot, on ? 3 : LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, on ? LV_OPA_COVER : LV_OPA_30, LV_PART_MAIN);
    }
}

bool PagerLoopEnabled(const PagerState* state) {
    return state != nullptr && state->page_count > 1;
}

// 无限循环翻页：pager 首尾各加一页克隆，布局为
// [末页克隆][真实页0..N-1][首页克隆]，真实页 i 的 scroll_x = (i+1)*kPanelSize。
int32_t PagerScrollXForPage(const PagerState* state, int logical_page) {
    if (state == nullptr) {
        return 0;
    }
    const int physical = PagerLoopEnabled(state) ? logical_page + 1 : logical_page;
    return static_cast<int32_t>(physical) * kPanelSize;
}

void PagerMaybeWrapAfterScroll(PagerState* state) {
    if (!PagerLoopEnabled(state) || state->pager == nullptr) {
        return;
    }
    const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
    if (scroll_x == 0) {
        const int last = state->page_count - 1;
        lv_obj_scroll_to_x(state->pager, PagerScrollXForPage(state, last),
                           LV_ANIM_OFF);
        HighlightDot(state, last);
        s_last_home_page = last;
    } else if (scroll_x == static_cast<int32_t>(state->page_count + 1) * kPanelSize) {
        lv_obj_scroll_to_x(state->pager, PagerScrollXForPage(state, 0),
                           LV_ANIM_OFF);
        HighlightDot(state, 0);
        s_last_home_page = 0;
    }
}

void OnPagerScrollBegin(lv_event_t* e) {
    lv_anim_t* a = lv_event_get_scroll_anim(e);
    if (a == nullptr) {
        return;
    }
    lv_anim_set_duration(a, kPageSlideAnimMs);
    lv_anim_set_path_cb(a, lv_anim_path_ease_out);
}

void OnPagerScrollEnd(lv_event_t* e) {
    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));
    if (state == nullptr || state->pager == nullptr) {
        return;
    }
    // 跟手拖动时 scroll_by(LV_ANIM_OFF) 每帧都会触发 SCROLL_END，
    // 不能在此处关骨架，否则手动拖时永远看不到骨架效果。
    if (s_home_touch.active && s_home_touch.paging) {
        return;
    }
    if (!lv_obj_is_scrolling(state->pager)) {
        PagerMaybeWrapAfterScroll(state);
        SetPagerSkeletonMode(state, false);
    }
}

// 翻页：平滑滚动到目标页；跟手拖动松手后也会调用以吸附到最近页。
void GoToPage(PagerState* state, int target_page) {
    if (state == nullptr || state->pager == nullptr) {
        return;
    }
    if (target_page < 0 || target_page >= state->page_count) {
        if (!PagerLoopEnabled(state)) {
            return;
        }
        target_page =
            (target_page % state->page_count + state->page_count) % state->page_count;
    }

    const int current = state->current_page;
    int32_t target_x = PagerScrollXForPage(state, target_page);
    if (PagerLoopEnabled(state)) {
        if (target_page == 0 && current == state->page_count - 1) {
            target_x = static_cast<int32_t>(state->page_count + 1) * kPanelSize;
        } else if (target_page == state->page_count - 1 && current == 0) {
            target_x = 0;
        }
    }

    const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
    if (target_page == current && scroll_x == target_x) {
        SetPagerSkeletonMode(state, false);
        return;
    }
    SetPagerSkeletonMode(state, true);
    lv_obj_scroll_to_x(state->pager, target_x, LV_ANIM_ON);
    HighlightDot(state, target_page);
    s_last_home_page = target_page;
    ResetHomeIdleTimer();
}

void SnapPagerToNearestPage(PagerState* state, int release_dx) {
    if (state == nullptr || state->pager == nullptr) {
        return;
    }
    const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
    const int anchor_x = PagerScrollXForPage(state, state->current_page);
    const int delta = static_cast<int>(scroll_x) - anchor_x;

    int target = state->current_page;
    // 相对起始页对称判定：左滑 delta>0 进下一页，右滑 delta<0 回上一页。
    if (delta > kPageSnapThreshold ||
        (release_dx <= -kHomeFlickThreshold && delta > kHomeMoveThreshold)) {
        target = state->current_page + 1;
    } else if (delta < -kPageSnapThreshold ||
               (release_dx >= kHomeFlickThreshold && delta < -kHomeMoveThreshold)) {
        target = state->current_page - 1;
    }

    if (target < 0) {
        if (PagerLoopEnabled(state) && state->current_page == 0) {
            GoToPage(state, state->page_count - 1);
            return;
        }
        target = 0;
    }
    if (target >= state->page_count) {
        if (PagerLoopEnabled(state) &&
            state->current_page == state->page_count - 1) {
            GoToPage(state, 0);
            return;
        }
        target = state->page_count - 1;
    }
    GoToPage(state, target);
}

// ---------------------------------------------------------------------------
// 触摸会话：PRESSED 锁定目标 → PRESSING/RELEASED 判滑动 → RELEASED 判单击/长按
//
//   |dx|、|dy| 均 < kHomeMoveThreshold：
//     按下→松开 < kHomeLongPressMs → Click（缩放 + launch）
//     按下→松开 ≥ kHomeLongPressMs → LongPress（仅缩放，预留）
//   |dx| >= |dy| 且主方向位移 >= kHomeAxisLockThreshold → 左右滑（PRESSING 跟手 / RELEASED 吸附）
//   |dy| > |dx| 且主方向位移 >= kHomeAxisLockThreshold → 上下滑（忽略，不隐藏图标）
//   方向未锁定前（消抖区）→ 不切换骨架、不跟手翻页
// ---------------------------------------------------------------------------

bool HomeTouchIsHorizontalSlide(int dx, int dy) {
    const int adx = std::abs(dx);
    const int ady = std::abs(dy);
    return adx >= kHomeAxisLockThreshold && adx * 2 > ady * 3;
}

bool HomeTouchIsVerticalSlide(int dx, int dy) {
    const int adx = std::abs(dx);
    const int ady = std::abs(dy);
    return ady >= kHomeAxisLockThreshold && ady * 2 > adx * 3;
}

void HomeTouchUpdateAxisLock(int dx, int dy) {
    if (s_home_touch.axis != HomeGestureAxis::None) {
        return;
    }
    if (HomeTouchIsHorizontalSlide(dx, dy)) {
        s_home_touch.axis = HomeGestureAxis::Horizontal;
        return;
    }
    if (HomeTouchIsVerticalSlide(dx, dy)) {
        s_home_touch.axis = HomeGestureAxis::Vertical;
        s_home_touch.consumed = true;
    }
}

bool HomeTouchIsTapLike(int dx, int dy) {
    return std::abs(dx) < kHomeMoveThreshold && std::abs(dy) < kHomeMoveThreshold;
}

HomeTouchKind HomeTouchClassifySwipe(int dx, int dy) {
    if (HomeTouchIsTapLike(dx, dy)) {
        return HomeTouchKind::None;
    }
    if (HomeTouchIsHorizontalSlide(dx, dy)) {
        return dx < 0 ? HomeTouchKind::SwipeLeft : HomeTouchKind::SwipeRight;
    }
    if (HomeTouchIsVerticalSlide(dx, dy)) {
        return dy < 0 ? HomeTouchKind::SwipeUp : HomeTouchKind::SwipeDown;
    }
    return HomeTouchKind::None;
}

void HomeTouchHandleSwipe(PagerState* state, HomeTouchKind kind) {
    if (state == nullptr) {
        return;
    }
    switch (kind) {
        case HomeTouchKind::SwipeLeft:
            GoToPage(state, state->current_page + 1);
            break;
        case HomeTouchKind::SwipeRight:
            GoToPage(state, state->current_page - 1);
            break;
        case HomeTouchKind::SwipeUp:
        case HomeTouchKind::SwipeDown:
            break;
        default:
            break;
    }
}

void HomeTouchDispatchTapLike(HomeTouchKind kind) {
    if (s_home_touch.press_cell == nullptr) {
        return;
    }
    if (kind == HomeTouchKind::Click) {
        if (s_home_touch.app == nullptr) {
            return;
        }
        PlayAppCellPressScale(s_home_touch.press_cell, s_home_touch.app);
        return;
    }
    if (kind == HomeTouchKind::LongPress) {
        PlayAppCellPressScale(s_home_touch.press_cell, nullptr);
    }
}

void HomeTouchTryStartPageDrag(PagerState* state, int dx, int dy, int current_x) {
    if (s_home_touch.axis != HomeGestureAxis::Horizontal) {
        return;
    }
    if (s_home_touch.consumed && !s_home_touch.paging) {
        return;
    }
    if (!HomeTouchIsHorizontalSlide(dx, dy)) {
        return;
    }
    if (!s_home_touch.paging) {
        s_home_touch.paging = true;
        s_home_touch.consumed = true;
        if (state != nullptr && state->pager != nullptr) {
            lv_obj_stop_scroll_anim(state->pager);
            SetPagerSkeletonMode(state, true);
        }
        s_home_touch.last_x = static_cast<int16_t>(current_x);
    }
}

void OnHomePressed(lv_event_t* e) {
    if (s_home_touch.active) {
        return;
    }
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_obj_t* screen = lv_event_get_current_target_obj(e);
    lv_obj_t* cell = FindAppCellFromTarget(lv_event_get_target_obj(e), screen);

    s_home_touch.active = true;
    s_home_touch.consumed = false;
    s_home_touch.paging = false;
    s_home_touch.axis = HomeGestureAxis::None;
    s_home_touch.start_x = static_cast<int16_t>(p.x);
    s_home_touch.start_y = static_cast<int16_t>(p.y);
    s_home_touch.last_x = static_cast<int16_t>(p.x);
    s_home_touch.press_tick = lv_tick_get();
    s_home_touch.press_cell = cell;
    s_home_touch.app =
        cell != nullptr
            ? static_cast<const AppEntry*>(lv_obj_get_user_data(cell))
            : nullptr;
    ResetHomeIdleTimer();
}

void OnHomePressing(lv_event_t* e) {
    if (!s_home_touch.active) {
        return;
    }
    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));
    lv_indev_t* indev = lv_event_get_indev(e);
    if (indev == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int dx = p.x - s_home_touch.start_x;
    const int dy = p.y - s_home_touch.start_y;
    if (!HomeTouchIsTapLike(dx, dy)) {
        ResetHomeIdleTimer();
    }

    HomeTouchUpdateAxisLock(dx, dy);

    // 上下滑或消抖区：不隐藏图标、不跟手翻页。
    if (s_home_touch.axis != HomeGestureAxis::Horizontal) {
        return;
    }

    if (state != nullptr) {
        SetPagerSkeletonMode(state, true);
    }

    if (!s_home_touch.consumed || s_home_touch.paging) {
        HomeTouchTryStartPageDrag(state, dx, dy, p.x);
    }

    if (s_home_touch.paging && state != nullptr && state->pager != nullptr) {
        const int delta_x = p.x - s_home_touch.last_x;
        if (delta_x != 0) {
            lv_obj_scroll_by(state->pager, delta_x, 0, LV_ANIM_OFF);
        }
        s_home_touch.last_x = static_cast<int16_t>(p.x);
    }
}

void OnHomeReleased(lv_event_t* e) {
    if (!s_home_touch.active) {
        return;
    }

    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));

    lv_indev_t* indev = lv_event_get_indev(e);
    int dx = 0;
    int dy = 0;
    if (indev != nullptr) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        dx = p.x - s_home_touch.start_x;
        dy = p.y - s_home_touch.start_y;
    }

    if (s_home_touch.axis == HomeGestureAxis::Vertical) {
        // 上下滑：不做任何处理，保持图标显示。
    } else if (s_home_touch.paging && state != nullptr) {
        SnapPagerToNearestPage(state, dx);
    } else if (!s_home_touch.consumed) {
        const uint32_t elapsed = lv_tick_elaps(s_home_touch.press_tick);
        if (HomeTouchIsTapLike(dx, dy)) {
            const HomeTouchKind kind =
                elapsed < kHomeLongPressMs ? HomeTouchKind::Click
                                           : HomeTouchKind::LongPress;
            HomeTouchDispatchTapLike(kind);
        } else {
            const HomeTouchKind kind = HomeTouchClassifySwipe(dx, dy);
            if (state != nullptr && kind != HomeTouchKind::None) {
                HomeTouchHandleSwipe(state, kind);
            }
        }
    }

    // 未进入跟手翻页且未吸附动画时，若仍在当前页则恢复图标。
    if (!s_home_touch.paging && state != nullptr && state->pager != nullptr &&
        !lv_obj_is_scrolling(state->pager)) {
        const int32_t scroll_x = lv_obj_get_scroll_x(state->pager);
        const int32_t expected = PagerScrollXForPage(state, state->current_page);
        if (scroll_x == expected) {
            SetPagerSkeletonMode(state, false);
        }
    }

    s_home_touch.active = false;
    s_home_touch.consumed = false;
    s_home_touch.paging = false;
    s_home_touch.axis = HomeGestureAxis::None;
    s_home_touch.press_cell = nullptr;
    s_home_touch.app = nullptr;
}

// 递归在子树上打 LV_OBJ_FLAG_EVENT_BUBBLE，让 cell / icon 上的 PRESSED /
// PRESSING / RELEASED 都能冒泡到屏幕被上面三个 handler 接住。
void EnableHomeEventBubble(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        EnableHomeEventBubble(lv_obj_get_child(obj, i));
    }
}

void OnHomeScreenLoaded(lv_event_t* e) {
    lv_obj_t* scr = lv_event_get_current_target_obj(e);
    EnableHomeEventBubble(scr);

    // 屏幕加载完成、layout 已经算好，这时再把 pager 滚到「上次停留的页」。
    // 直接在 Create() 末尾 scroll_to_x 经常因为 layout 还没算赶不上首帧。
    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));
    if (state != nullptr && state->pager != nullptr) {
        lv_obj_update_layout(state->pager);
        int page = s_last_home_page;
        if (page < 0 || page >= state->page_count) {
            page = 0;
        }
        lv_obj_scroll_to_x(state->pager, PagerScrollXForPage(state, page),
                           LV_ANIM_OFF);
        HighlightDot(state, page);
    }

    PwrKey_OnScreenLifecycle("home", SCREEN_LIFECYCLE_LOAD);
    StartHomeIdleTimer();
}

void OnHomeScreenUnloaded(lv_event_t* /*e*/) {
    // 离开首页必须出栈，否则 Test→二级页时父页 UNLOAD 后栈底仍残留 home，
    // 若二级页未登记会误把前台当成首页从而进待机。
    PwrKey_OnScreenLifecycle("home", SCREEN_LIFECYCLE_UNLOAD);
}

void OnScreenDeleted(lv_event_t* e) {
    CancelCellScaleTimer();
    StopHomeIdleTimer();
    auto* state = static_cast<PagerState*>(lv_event_get_user_data(e));
    if (s_active_pager == state) {
        s_active_pager = nullptr;
    }
    if (s_home_scr == lv_event_get_target_obj(e)) {
        s_home_scr = nullptr;
    }
    delete state;
}

// ---------------------------------------------------------------------------
// 电源对话框 (由 PwrKey_Init 长按回调经 lv_async_call 调起)
//
// 长按 PWR_KEY 弹出居中模态框，里面两个大按钮：重启 / 关机。
// 点击空白处（mask 自身，不在 card 区域内）关闭对话框。
//
// 线程模型：
//   - pwr_key_handler 在 IOExpander monitor task 里检测到长按后通过
//     lv_async_call 把 ShowPowerOptionsDialog 调度到 LVGL 线程执行。
//   - 关机操作要发 PWR_KEY_PULSE 持续脉冲，单开 FreeRTOS task 完成；
//     不阻塞 LVGL 线程，也不污染 IOExpander 的 monitor。
//
// EVENT_BUBBLE 默认关闭，所以 card / 按钮上的点击不会冒泡到 mask 触发
// 误关闭；为了给后续修改做兜底，OnPwrMaskClicked 里再做一次 target 校验。
// ---------------------------------------------------------------------------
struct PowerDialogUi {
    lv_obj_t* mask = nullptr;
    lv_obj_t* card = nullptr;
};

PowerDialogUi s_pwr_dlg;

void ClosePowerDialog() {
    if (s_pwr_dlg.mask != nullptr) {
        lv_obj_delete(s_pwr_dlg.mask);
    }
    s_pwr_dlg = PowerDialogUi{};
}

bool TryClosePowerDialogInternal() {
    if (s_pwr_dlg.mask == nullptr) {
        return false;
    }
    ClosePowerDialog();
    return true;
}

void OnPwrMaskClicked(lv_event_t* e);  // forward decl

lv_obj_t* s_shutdown_screen = nullptr;

void AppendShutdownProgressContent(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_row(box, 24, LV_PART_MAIN);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* spin = lv_spinner_create(box);
    lv_obj_set_size(spin, 48, 48);
    lv_spinner_set_anim_params(spin, 1000, 200);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_INDICATOR);
    lv_obj_remove_flag(spin, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(box);
    lv_label_set_text(lbl, I18n::T("正在关机..."));
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* CreateShutdownScreen() {
    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, kPanelSize, kPanelSize);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    AppendShutdownProgressContent(screen);
    return screen;
}

// 切到独立黑底关机关机页，避免在原页面半透明遮罩上显示。
void ShowShutdownScreen() {
    ClosePowerDialog();

    if (s_shutdown_screen == nullptr) {
        s_shutdown_screen = CreateShutdownScreen();
    }
    lv_screen_load(s_shutdown_screen);
}

bool s_system_shutting_down = false;
lv_obj_t* s_usb_block_dlg = nullptr;

void CloseUsbBlockDialog() {
    if (s_usb_block_dlg != nullptr) {
        lv_obj_delete(s_usb_block_dlg);
        s_usb_block_dlg = nullptr;
    }
}

void OnUsbBlockOkClicked(lv_event_t* /*e*/) {
    CloseUsbBlockDialog();
}

void ShowUsbPowerOffBlockedDialog() {
    lv_obj_t* scr = lv_screen_active();
    if (scr == nullptr) {
        return;
    }
    CloseUsbBlockDialog();

    constexpr int kCardW = 210;
    constexpr int kBtnW = 140;
    constexpr int kBtnH = 32;

    lv_obj_t* mask = lv_obj_create(scr);
    lv_obj_remove_style_all(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelSize, kPanelSize);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_usb_block_dlg = mask;

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, kCardW);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(card, true);

    auto add_line = [&](const char* text, uint32_t color) {
        lv_obj_t* lbl = lv_label_create(card);
        lv_label_set_text(lbl, I18n::T(text));
        lv_obj_set_width(lbl, kCardW - 28);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_16_4, LV_PART_MAIN);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    };
    add_line("USB供电中", 0xFFFFFF);
    add_line("软件关机失败", 0xFFFFFF);
    add_line("请拔掉USB后重试，或长按关机键强制关机", 0x9AA3B2);

    lv_obj_t* ok = lv_button_create(card);
    lv_obj_remove_style_all(ok);
    lv_obj_set_size(ok, kBtnW, kBtnH);
    lv_obj_set_style_bg_color(ok, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ok, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(ok, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(ok, OnUsbBlockOkClicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(ok, true);

    lv_obj_t* ok_lbl = lv_label_create(ok);
    lv_label_set_text(ok_lbl, I18n::T("确定"));
    lv_obj_set_style_text_color(ok_lbl, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_text_font(ok_lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_center(ok_lbl);
    lv_obj_remove_flag(ok_lbl, LV_OBJ_FLAG_CLICKABLE);
}

void AbortSystemShutdownAsync(void* /*user_data*/) {
    s_system_shutting_down = false;
    ESP_LOGW(TAG_HOME, "软件关机被拒绝（如 USB 供电），恢复首页并提示");

    lv_obj_t* old = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old != nullptr && old != home && old == s_shutdown_screen) {
        s_shutdown_screen = nullptr;
        lv_obj_delete_async(old);
    } else if (s_shutdown_screen != nullptr && s_shutdown_screen != home) {
        lv_obj_delete_async(s_shutdown_screen);
        s_shutdown_screen = nullptr;
    }
    IdlePower_Attach(IdlePowerSession::Home, /*reset_activity=*/true);
    ShowUsbPowerOffBlockedDialog();
}

// 关机脉冲 task：调用板级关机；USB 供电时板级会拒绝并返回，由 LVGL 恢复 UI。
void PwrShutdownPulseTask(void* /*arg*/) {
    const bool started = board_request_shutdown();
    if (!started) {
        lv_async_call(AbortSystemShutdownAsync, nullptr);
    }
    vTaskDelete(nullptr);
}

void BeginSystemShutdown(const char* reason) {
    if (s_system_shutting_down) {
        return;
    }
    s_system_shutting_down = true;

    ESP_LOGW(TAG_HOME, "%s：开始关机", reason);
    IdlePower_Stop();
    ShowShutdownScreen();
    // 2048 对 ESP_LOG / C++ 路径偏紧，曾在 USB 拒关机时栈溢出重启。
    xTaskCreate(PwrShutdownPulseTask, "pwr_off_pulse", 4096, nullptr, 5, nullptr);
}

void OnPwrShutdownClicked(lv_event_t* /*e*/) {
    BeginSystemShutdown(I18n::T("用户选择 [关机]"));
}

void OnPwrRebootClicked(lv_event_t* /*e*/) {
    ESP_LOGW(TAG_HOME, "用户选择 [重启]：调用 Application::Reboot()");
    ClosePowerDialog();
    Application::GetInstance().Reboot();
}

void OnPwrMaskClicked(lv_event_t* e) {
    // 只处理「点在 mask 自身」的事件。card / 按钮的点击因为 EVENT_BUBBLE
    // 默认关闭根本不会冒泡到这里；这里再做一道 target 校验是为了万一以后
    // 谁动了 EVENT_BUBBLE 标志也不会误关。
    if (lv_event_get_target_obj(e) != lv_event_get_current_target_obj(e)) {
        return;
    }
    ESP_LOGI(TAG_HOME, "点击模态框外，关闭电源对话框");
    ClosePowerDialog();
}

lv_obj_t* CreatePowerActionBtn(lv_obj_t* parent,
                               const char* icon_src,
                               const char* text,
                               lv_event_cb_t on_click) {
    constexpr int kBtnSize = 56;
    constexpr int kBtnIconSize = 28;  // 与 ic_s_home_*.png 一致，1:1

    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kBtnSize, kBtnSize);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);

    lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_10, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 24, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t* icon = lv_image_create(btn);
    lv_image_set_src(icon, icon_src);
    lv_obj_set_size(icon, kBtnIconSize, kBtnIconSize);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_pad_top(lbl, 2, LV_PART_MAIN);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, nullptr);
    return btn;
}

void ShowPowerDialog() {
    if (s_pwr_dlg.mask != nullptr) {
        return;  // 已经打开
    }
    lv_obj_t* parent = lv_screen_active();
    if (parent == nullptr) {
        return;
    }

    // ---- 全屏遮罩 ----
    // FLOATING：让 mask 脱离父屏的 flex / grid 布局。
    // 比如 gps_screen 在根对象上挂了 LV_FLEX_FLOW_COLUMN，没有这个 flag
    // 时 mask 会被父屏的 flex 接管 —— `lv_obj_set_pos(0,0)` 会被布局覆盖
    // 重排到列尾，加上 720x720 比屏幕还大就只能看到底部一截。挂了
    // FLOATING 后 mask 完全被父布局忽略，set_pos 重新生效，dialog 在任何
    // 屏幕上都能正确居中铺满。
    lv_obj_t* mask = lv_obj_create(parent);
    lv_obj_remove_style_all(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(mask, kPanelSize, kPanelSize);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_70, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    lv_obj_add_event_cb(mask, OnPwrMaskClicked, LV_EVENT_CLICKED, nullptr);
    s_pwr_dlg.mask = mask;

    // ---- 中心卡片 ----
    constexpr int kCardW = 200;
    constexpr int kCardH = 168;
    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, kCardW, kCardH);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B2030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 24, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // card 必须 clickable —— 这样点中 card 范围内的事件被 card 自己消费，
    // 不会落到 mask 触发关闭逻辑。
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    s_pwr_dlg.card = card;

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, I18n::T("电源选项"));
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    // ---- 按钮行 ----
    lv_obj_t* row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    CreatePowerActionBtn(row, "A:ic_s_home_reboot.spng", I18n::T("重启"),
                         OnPwrRebootClicked);
    CreatePowerActionBtn(row, "A:ic_s_home_power.spng", I18n::T("关机"),
                         OnPwrShutdownClicked);

    // ---- 底部提示：硬件强制关机说明 ----
    lv_obj_t* hint = lv_label_create(card);
    lv_label_set_text(hint, I18n::T("长按关机键 5 秒可强制关机"));
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9CA3AF), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* CreatePage(lv_obj_t* pager, int page_index, int total_apps) {
    lv_obj_t* page = lv_obj_create(pager);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, kPanelSize, kPagerHeight);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    if (page_index >= 0 && page_index < total_apps) {
        const AppEntry& app = kApps[page_index];
        if (app.icon_suffix != nullptr) {
            CreateAppCell(page, app, page_index);
        }
    }
    return page;
}

void CreateIndicator(lv_obj_t* screen, PagerState* state) {
    // 对齐 HTML：无胶囊底，仅一排小圆点；当前点拉长为胶囊。
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
    state->indicator = indicator;

    for (int i = 0; i < state->page_count; ++i) {
        lv_obj_t* dot = lv_obj_create(indicator);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, kDotSize, kDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(kDotColor), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_30, LV_PART_MAIN);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        state->dots[i] = dot;
    }

    HighlightDot(state, 0);
}

}  // namespace

void HomeScreen::ShowPowerOptionsDialog() { ShowPowerDialog(); }

bool HomeScreen::TryClosePowerDialog() { return TryClosePowerDialogInternal(); }

lv_obj_t* HomeScreen::Create() {
    // 主题相关：根据 NVS 里的当前主题 id 把 kApps 的 icon_suffix 拼成完整
    // 路径，写入 s_icon_paths 缓存。后续 CreateAppCell 直接索引这份缓存。
    EnsureIconPathsBuilt();

    lv_obj_t* screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // ----- Figure out how many pages we need -----
    // kTotalApps 已经在 namespace 作用域里声明，直接复用。
    int page_count = (kTotalApps + kAppsPerPage - 1) / kAppsPerPage;
    if (page_count < 1)
        page_count = 1;
    if (page_count > kMaxPages)
        page_count = kMaxPages;

    // PagerState owns the dot pointers + current_page; freed on screen del.
    auto* state = new PagerState{};
    state->page_count = page_count;
    state->current_page = 0;

    auto* status = new HomeStatusState{};
    CreateStatusBar(screen, status);

    // ----- Pager（左右滑动翻页容器） -----
    // 不开启 LVGL 原生 scrollable，避免与 screen 级触摸分发抢手势。
    // 水平滑动时由 OnHomePressing 跟手拖动 pager；松手或快速 fling 时
    // 通过 lv_obj_scroll_to_x(..., LV_ANIM_ON) 平滑吸附到目标页。
    // 图标在 CreateAppCell 时预光栅化到 PSRAM，滑动不再重解 SPNG。
    lv_obj_t* pager = lv_obj_create(screen);
    state->pager = pager;
    lv_obj_remove_style_all(pager);
    lv_obj_set_size(pager, kPanelSize, kPagerHeight);
    lv_obj_align(pager, LV_ALIGN_TOP_LEFT, 0, kStatusBarHeight);
    lv_obj_set_style_bg_opa(pager, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(pager, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(pager, LV_SCROLLBAR_MODE_OFF);
    // Row flex；每页固定宽度并列排布，pager 的 scroll 偏移由我们手动控制。
    lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pager, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(pager, OnPagerScrollBegin, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_add_event_cb(pager, OnPagerScrollEnd, LV_EVENT_SCROLL_END, state);

    if (page_count > 1) {
        // 首尾克隆页实现无限循环：末页克隆 | 真实页 | 首页克隆
        CreatePage(pager, page_count - 1, kTotalApps);
        for (int p = 0; p < page_count; ++p) {
            CreatePage(pager, p, kTotalApps);
        }
        CreatePage(pager, 0, kTotalApps);
    } else {
        CreatePage(pager, 0, kTotalApps);
    }

    // ----- Page indicator -----
    // Only worth drawing when there is more than one page; otherwise it's
    // a lonely single dot which just adds noise.
    if (page_count > 1) {
        CreateIndicator(screen, state);
    }

    // ----- 触摸：screen 级 PRESSED / PRESSING / RELEASED 统一分类分发 -----
    // LV_EVENT_SCREEN_LOADED 后再铺 EVENT_BUBBLE，让子树按压事件冒泡到 screen。
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, OnHomePressed, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(screen, OnHomePressing, LV_EVENT_PRESSING, state);
    lv_obj_add_event_cb(screen, OnHomeReleased, LV_EVENT_RELEASED, state);
    lv_obj_add_event_cb(screen, OnHomeScreenLoaded, LV_EVENT_SCREEN_LOADED, state);
    lv_obj_add_event_cb(screen, OnHomeScreenUnloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    lv_obj_add_event_cb(screen, OnScreenDeleted, LV_EVENT_DELETE, state);

    s_home_scr = screen;
    s_active_pager = state;

    return screen;
}

void OnRefreshStatusBarAsync(void* /*user_data*/) {
    if (s_home_status != nullptr) {
        UpdateHomeStatusBar(s_home_status);
    }
}

void HomeScreen::ResetToFirstPage() {
    s_last_home_page = 0;
}

bool HomeScreen::IsActive() {
    return s_home_scr != nullptr && lv_screen_active() == s_home_scr;
}

void HomeScreen::FlipPage(int dir) {
    TryClosePowerDialogInternal();
    if (!IsActive() || s_active_pager == nullptr || dir == 0) {
        return;
    }
    const int n = s_active_pager->page_count;
    if (n <= 0) {
        return;
    }
    int target = s_active_pager->current_page + dir;
    if (PagerLoopEnabled(s_active_pager)) {
        if (target < 0) {
            target = n - 1;
        } else if (target >= n) {
            target = 0;
        }
    } else {
        if (target < 0 || target >= n) {
            return;
        }
    }
    GoToPage(s_active_pager, target);
}

void HomeScreen::GoHomeToChat() {
    TryClosePowerDialogInternal();
    ResetToFirstPage();
    if (IsActive() && s_active_pager != nullptr) {
        GoToPage(s_active_pager, 0);
        return;
    }
    lv_obj_t* old = lv_screen_active();
    lv_obj_t* home = HomeScreen::Create();
    lv_screen_load(home);
    if (old != nullptr && old != home) {
        lv_obj_delete_async(old);
    }
}

void HomeScreen::RefreshStatusBar() {
    // Application::CheckNewVersion / ShowActivationCode 跑在 app_main
    // 线程，不能直接碰 LVGL；切到 LVGL 线程再刷新，避免与 adapter 争用锁
    // 导致 lv_obj_invalidate 在 Core0 上死循环占满 CPU。
    lv_async_call(OnRefreshStatusBarAsync, nullptr);
}

int HomeScreen::GetIdleShutdownMinutes() {
    return IdlePower_GetShutdownMinutes();
}

void HomeScreen::SetIdleShutdownMinutes(int minutes) {
    IdlePower_SetShutdownMinutes(minutes);
    IdlePower_NotifyActivity();
}

int HomeScreen::GetIdleStandbyMinutes() {
    return IdlePower_GetStandbyMinutes();
}

void HomeScreen::SetIdleStandbyMinutes(int minutes) {
    IdlePower_SetStandbyMinutes(minutes);
    IdlePower_NotifyActivity();
}

void HomeScreen::RequestSystemShutdown(const char* reason) {
    BeginSystemShutdown(reason != nullptr ? reason : I18n::T("系统关机"));
}
