#include "network_screen.h"
#include "i18n.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "ssid_manager.h"
#include "wifi_manager.h"

#include "application.h"
#include "board.h"
#include "dual_network_board.h"
#include "settings.h"

#include "home_screen/home_screen.h"
#include "panel_metrics.h"
#include "screen_util.h"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_puhui_30_4);

namespace {

constexpr const char* TAG = "NetworkScreen";

// ---------------------------------------------------------------------------
// 视觉常量
// ---------------------------------------------------------------------------
constexpr int kPanelW = panel::kWidth;
constexpr int kPanelH = panel::kHeight;
constexpr int kHeaderH = panel::kHeaderH;

constexpr uint32_t kColorBg         = 0x0E1116;
constexpr uint32_t kColorCard       = 0x1B2030;
constexpr uint32_t kColorBtn        = 0x2A2F3A;
constexpr uint32_t kColorBtnActive  = 0x3B82F6;
constexpr uint32_t kColorBtnDanger  = 0xDC2626;
constexpr uint32_t kColorBtnAccent  = 0x10B981;
constexpr uint32_t kColorText       = 0xFFFFFF;
constexpr uint32_t kColorSubtle     = 0x9AA3B2;
constexpr uint32_t kColorSuccess    = 0x34C759;
constexpr uint32_t kColorError      = 0xFF3B30;
constexpr uint32_t kColorScanning   = 0xF59E0B;
constexpr uint32_t kColorListBg     = 0x12151C;
constexpr uint32_t kColorItem       = 0x202736;
constexpr uint32_t kColorItemSel    = 0x2F3A52;

constexpr size_t kMaxSsidLen = 32;
constexpr size_t kMaxPasswordLen = 64;

// ---------------------------------------------------------------------------
// FreeRTOS / WiFi 事件位
// ---------------------------------------------------------------------------
constexpr EventBits_t kBitScanDone     = BIT0;
constexpr EventBits_t kBitConnected    = BIT1;
constexpr EventBits_t kBitDisconnected = BIT2;

// ---------------------------------------------------------------------------
// 数据
// ---------------------------------------------------------------------------
struct ApItem {
    std::string ssid;
    int8_t rssi = -127;
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
};

struct UiState {
    lv_obj_t* screen        = nullptr;
    lv_obj_t* status_label  = nullptr;
    lv_obj_t* scan_btn      = nullptr;
    lv_obj_t* scan_btn_lbl  = nullptr;
    lv_obj_t* tabview       = nullptr;
    lv_obj_t* nearby_tab    = nullptr;  // tabview 的 tab 容器
    lv_obj_t* saved_tab     = nullptr;
    lv_obj_t* network_tab   = nullptr;
    lv_obj_t* sim_tab       = nullptr;
    lv_obj_t* nearby_list   = nullptr;  // 实际放 item 的 flex 容器（挂在扫描 sheet 内）
    lv_obj_t* nearby_spinner = nullptr; // 扫描中悬浮在列表中央的圆环 spinner
    lv_obj_t* saved_list    = nullptr;
    lv_obj_t* saved_sheet   = nullptr;
    lv_obj_t* saved_view_btn = nullptr;
    lv_obj_t* saved_count_lbl = nullptr;
    // 扫描配网全屏 sheet（列表 → 点 AP 再出密码层）
    lv_obj_t* scan_sheet      = nullptr;
    lv_obj_t* sheet_title_lbl = nullptr;
    lv_obj_t* sheet_rescan_btn = nullptr;
    lv_obj_t* sheet_rescan_lbl = nullptr;
    // 上网方式切换（WiFi / 4G）：和 SIM 卡切换一致的两按钮选择器
    lv_obj_t* network_wifi_btn    = nullptr;
    lv_obj_t* network_wifi_lbl    = nullptr;
    lv_obj_t* network_cell_btn    = nullptr;
    lv_obj_t* network_cell_lbl    = nullptr;
    lv_obj_t* network_current_lbl = nullptr;
    // SIM 卡切换（仅 4G 模式）
    lv_obj_t* sim_external_btn    = nullptr;
    lv_obj_t* sim_external_lbl    = nullptr;
    lv_obj_t* sim_internal_btn    = nullptr;
    lv_obj_t* sim_internal_lbl    = nullptr;
    lv_obj_t* sim_current_lbl     = nullptr;
    // 密码键盘弹窗
    lv_obj_t* pwd_overlay   = nullptr;
    lv_obj_t* pwd_textarea  = nullptr;
    lv_obj_t* pwd_keyboard  = nullptr;
    lv_obj_t* pwd_title     = nullptr;
    lv_obj_t* pwd_show_chk  = nullptr;
    // 连接进度 / 成功提示弹窗（盖在 pwd_overlay 之上的全屏模态）
    lv_obj_t* status_overlay     = nullptr;
    lv_obj_t* status_message_lbl = nullptr;
};

UiState s_ui;

std::vector<ApItem>  s_scan_results;
bool                 s_screen_active = false;
bool                 s_wifi_initialized = false;
bool                 s_scan_in_progress = false;
bool                 s_connect_in_progress = false;
std::string          s_pending_ssid;        // 当前正在弹键盘 / 连接的 SSID
wifi_auth_mode_t     s_pending_authmode = WIFI_AUTH_OPEN;

esp_netif_t*         s_netif = nullptr;
bool                 s_netif_owned = false;  // 本页 create 的才 destroy，复用已有则否
esp_event_handler_instance_t s_wifi_evt_inst = nullptr;
esp_event_handler_instance_t s_ip_evt_inst   = nullptr;
EventGroupHandle_t   s_evt_group = nullptr;
// 最近一次 STA_DISCONNECTED 事件携带的 reason 码，用来判断是密码错误还是 AP
// 拒绝等。0 表示「没拿到原因」。
uint8_t              s_last_disconnect_reason = 0;

// 重启倒计时（连接成功后用）。kRestartCountdownSec 秒后自动 esp_restart。
constexpr int        kRestartCountdownSec = 3;
lv_timer_t*          s_restart_timer = nullptr;
int                  s_restart_remaining = 0;
std::string          s_restart_headline;
// 记录进入页面前 WifiStation 是否已经在跑（即设备网络模式是 WiFi），
// 用来决定离开时是否恢复 WifiStation::Start()。ML307 模式下 WifiStation
// 根本没起过，恢复时跳过即可，避免空跑一份 wifi 栈。
bool                 s_wifi_station_was_active = false;
// Only scanning/connecting transfers the driver from WifiManager to this page.
bool                 s_wifi_taken_over = false;
bool                 s_network_switch_pending = false;

// 上网方式（network/type NVS key）：与 DualNetworkBoard::LoadNetworkTypeFromSettings
// 一致。0 = WiFi，1 = 4G（蜂窝模组）。切换由 DualNetworkBoard::SwitchNetworkType()
// 主导，并触发设备重启。
constexpr int        kNetTypeWifi     = 0;
constexpr int        kNetTypeCellular = 1;

// SIM 卡相关。
//   - kSimSlotExternal = 0：外置卡（默认）
//   - kSimSlotInternal = 1：内置卡
// 切换通过 AT+ECSIMCFG=SimSlot,X 完成，需要 AT+CFUN=0 / AT+CFUN=1 包夹。
// s_sim_switch_pending 用来在切换期间禁止重复按按钮。
constexpr int        kSimSlotExternal = 0;
constexpr int        kSimSlotInternal = 1;
bool                 s_sim_switch_pending = false;

// 前向声明
void post_status(const char* text, uint32_t color = kColorText);
void refresh_saved_list();
void refresh_nearby_list();
void rebuild_nearby_list_now();
void rebuild_saved_list_now();
void open_password_popup(const std::string& ssid, wifi_auth_mode_t authmode);
void close_password_popup();
void open_scan_sheet();
void close_scan_sheet();
void open_saved_sheet();
void close_saved_sheet();
void schedule_scan();
void update_saved_count_now();
void schedule_connect(const std::string& ssid, const std::string& password);
void open_connecting_popup(const std::string& ssid);
void open_restart_countdown_popup(const std::string& headline);
void open_success_popup(const std::string& ssid);
void show_failure_in_status_popup(const std::string& title,
                                  const std::string& detail,
                                  uint32_t auto_close_ms = 2500);
void close_status_popup();
void refresh_network_switch_ui();
void open_switch_reboot_popup(const char* target_name);
void schedule_network_switch(int target_type);
void refresh_sim_slot_ui();
void open_sim_switching_popup(int target_slot);
void schedule_sim_slot_query();

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
const char* auth_label(wifi_auth_mode_t mode) {
    return (mode == WIFI_AUTH_OPEN) ? I18n::T("[开放]") : I18n::T("[加密]");
}

// 把 STA_DISCONNECTED 的 reason 码翻成人话。常见值对应密码错误 / AP 找不到 /
// 鉴权 / 关联超时等几大类。
const char* disconnect_reason_text(uint8_t reason) {
    switch (reason) {
        // —— 密码 / 鉴权类 —— 用户最关心的「密码错了」全在这里
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_AUTH_LEAVE:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        case WIFI_REASON_GROUP_CIPHER_INVALID:
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        case WIFI_REASON_AKMP_INVALID:
        case WIFI_REASON_802_1X_AUTH_FAILED:
            return I18n::T("密码错误，请重新输入");
        case WIFI_REASON_NO_AP_FOUND:
            return I18n::T("未找到该 WiFi（信号丢失）");
        case WIFI_REASON_ASSOC_EXPIRE:
        case WIFI_REASON_ASSOC_TOOMANY:
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_NOT_ASSOCED:
            return I18n::T("关联失败，路由器拒绝连接");
        case WIFI_REASON_BEACON_TIMEOUT:
            return I18n::T("信号太弱，连接超时");
        case 0:
            return I18n::T("连接失败");
        default:
            return nullptr;  // 调用方自己拼 reason=xx
    }
}

const char* rssi_quality_text(int8_t rssi) {
    if (rssi >= -55) return I18n::T("信号强");
    if (rssi >= -65) return I18n::T("信号较强");
    if (rssi >= -75) return I18n::T("信号中");
    if (rssi >= -85) return I18n::T("信号弱");
    return I18n::T("信号很弱");
}

bool screen_alive() { return s_screen_active && s_ui.screen != nullptr; }

int GetSavedNetworkType() {
    // 与 DualNetworkBoard 启动时读 NVS 的逻辑一致（默认 4G=1，）
    const NetworkType type =
        DualNetworkBoard::LoadNetworkTypeFromSettings(kNetTypeCellular);
    return type == NetworkType::ML307 ? kNetTypeCellular : kNetTypeWifi;
}

DualNetworkBoard* GetDualNetworkBoard() {
    return dynamic_cast<DualNetworkBoard*>(&Board::GetInstance());
}

// 当前是否处于 4G（蜂窝）模式。Settings 中 "network/type" 的语义：
// 0 = WiFi、1 = 4G/ML307。4G 模式下我们不展示「附近 WiFi」「已保存 WiFi」
// 两个 Tab，也不会启动本地 STA 栈做扫描。
bool IsCellularMode() {
    return GetSavedNetworkType() == 1;
}

// 1.54 使用 ML307，无 NT26 双 SIM AT 通道。
bool HasDualSimModem() { return false; }

// 读取/写入用户选择的 SIM 槽位。默认 0 = 外置卡。这个值只用于 UI 高亮，
// 模组本身的当前槽位由 AT+ECSIMCFG 命令直接持久化在模组里。
int GetSavedSimSlot() {
    Settings settings("network", true);
    int v = settings.GetInt("sim_slot", kSimSlotExternal);
    return (v == kSimSlotInternal) ? kSimSlotInternal : kSimSlotExternal;
}

void SaveSimSlot(int slot) {
    Settings settings("network", true);
    settings.SetInt("sim_slot", slot);
}

const char* SimSlotName(int slot) {
    return (slot == kSimSlotInternal) ? I18n::T("内置卡") : I18n::T("外置卡");
}

// ---------------------------------------------------------------------------
// 异步 UI 更新（worker task -> LVGL 线程）
// ---------------------------------------------------------------------------
struct AsyncStatusMsg {
    char text[160];
    uint32_t color;
};

void async_update_status(void* user_data) {
    auto* msg = static_cast<AsyncStatusMsg*>(user_data);
    if (screen_alive() && s_ui.status_label != nullptr) {
        lv_label_set_text(s_ui.status_label, msg->text);
        if (msg->text[0] == '\0') {
            lv_obj_add_flag(s_ui.status_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_ui.status_label, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_text_color(s_ui.status_label, lv_color_hex(msg->color), LV_PART_MAIN);
    }
    delete msg;
}

void post_status(const char* text, uint32_t color) {
    if (!s_screen_active) return;
    auto* msg = new AsyncStatusMsg{};
    snprintf(msg->text, sizeof(msg->text), "%s", text);
    msg->color = color;
    lv_async_call(async_update_status, msg);
}

void async_rebuild_nearby(void* /*user_data*/) {
    if (screen_alive()) {
        rebuild_nearby_list_now();
    }
}

void async_rebuild_saved(void* /*user_data*/) {
    if (screen_alive()) {
        rebuild_saved_list_now();
    }
}

void refresh_nearby_list() {
    if (!s_screen_active) return;
    lv_async_call(async_rebuild_nearby, nullptr);
}

void refresh_saved_list() {
    if (!s_screen_active) return;
    lv_async_call(async_rebuild_saved, nullptr);
}

void async_set_scan_btn_enabled(void* user_data) {
    const bool enabled = (user_data != nullptr);
    auto apply = [&](lv_obj_t* btn, lv_obj_t* lbl, const char* idle_text) {
        if (btn == nullptr) return;
        if (enabled) {
            lv_obj_remove_state(btn, LV_STATE_DISABLED);
            if (lbl != nullptr) lv_label_set_text(lbl, idle_text);
        } else {
            lv_obj_add_state(btn, LV_STATE_DISABLED);
            if (lbl != nullptr) lv_label_set_text(lbl, I18n::T("扫描中"));
        }
    };
    if (!screen_alive()) return;
    apply(s_ui.scan_btn, s_ui.scan_btn_lbl, I18n::T("扫描附近网络"));
    apply(s_ui.sheet_rescan_btn, s_ui.sheet_rescan_lbl, I18n::T("重扫"));
    if (s_ui.sheet_title_lbl != nullptr) {
        lv_label_set_text(s_ui.sheet_title_lbl,
                          enabled ? I18n::T("附近 Wi-Fi") : I18n::T("扫描中"));
    }
}

void post_scan_btn_enabled(bool enabled) {
    if (!s_screen_active) return;
    lv_async_call(async_set_scan_btn_enabled,
                  reinterpret_cast<void*>(static_cast<intptr_t>(enabled ? 1 : 0)));
}

// ---- 「附近 WiFi」列表中央的圆环 spinner --------------------------------
// scan_task 在初始化 / 扫描期间打开，列表填充完成 / 出错时关闭。spinner 自
// 身用 LVGL 的内置动画，不需要我们手动驱动。
void async_set_nearby_spinner(void* user_data) {
    if (!screen_alive() || s_ui.nearby_spinner == nullptr) return;
    const bool show = (user_data != nullptr);
    if (show) {
        lv_obj_remove_flag(s_ui.nearby_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ui.nearby_spinner);
    } else {
        lv_obj_add_flag(s_ui.nearby_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

void post_nearby_spinner(bool show) {
    if (!s_screen_active) return;
    lv_async_call(async_set_nearby_spinner,
                  reinterpret_cast<void*>(static_cast<intptr_t>(show ? 1 : 0)));
}

struct AsyncStringMsg {
    std::string text;
};

void async_open_connecting(void* user_data) {
    auto* msg = static_cast<AsyncStringMsg*>(user_data);
    if (screen_alive()) open_connecting_popup(msg->text);
    delete msg;
}

void post_open_connecting(const std::string& ssid) {
    if (!s_screen_active) return;
    lv_async_call(async_open_connecting, new AsyncStringMsg{ssid});
}

void async_open_success_and_reboot(void* user_data) {
    auto* msg = static_cast<AsyncStringMsg*>(user_data);
    if (screen_alive()) {
        // 成功后把密码弹窗也关掉，让 success 弹窗成为唯一顶层 UI
        close_password_popup();
        open_success_popup(msg->text);
    }
    delete msg;
}

void post_open_success_and_reboot(const std::string& ssid) {
    if (!s_screen_active) return;
    lv_async_call(async_open_success_and_reboot, new AsyncStringMsg{ssid});
}

void async_close_status(void* /*user_data*/) {
    if (screen_alive()) close_status_popup();
}

void post_close_status_popup() {
    if (!s_screen_active) return;
    lv_async_call(async_close_status, nullptr);
}

struct AsyncFailureMsg {
    std::string title;
    std::string detail;
    uint32_t    auto_close_ms;
};

void async_show_failure(void* user_data) {
    auto* msg = static_cast<AsyncFailureMsg*>(user_data);
    if (screen_alive()) {
        show_failure_in_status_popup(msg->title, msg->detail, msg->auto_close_ms);
    }
    delete msg;
}

void post_show_failure(const std::string& title, const std::string& detail,
                       uint32_t auto_close_ms = 2500) {
    if (!s_screen_active) return;
    lv_async_call(async_show_failure,
                  new AsyncFailureMsg{title, detail, auto_close_ms});
}

// ---------------------------------------------------------------------------
// WiFi 事件
// ---------------------------------------------------------------------------
void wifi_evt_handler(void* /*arg*/, esp_event_base_t base, int32_t id,
                      void* data) {
    if (s_evt_group == nullptr) return;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_SCAN_DONE) {
            xEventGroupSetBits(s_evt_group, kBitScanDone);
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            auto* evt = static_cast<wifi_event_sta_disconnected_t*>(data);
            s_last_disconnect_reason = (evt != nullptr) ? evt->reason : 0;
            ESP_LOGW(TAG, "STA_DISCONNECTED reason=%u", s_last_disconnect_reason);
            xEventGroupSetBits(s_evt_group, kBitDisconnected);
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            xEventGroupSetBits(s_evt_group, kBitConnected);
        }
    }
}

// 失败路径清理：避免留下 netif / 半初始化 wifi，导致重扫时 duplicate key assert。
void wifi_init_cleanup_partial() {
    if (s_wifi_evt_inst != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_evt_inst);
        s_wifi_evt_inst = nullptr;
    }
    if (s_ip_evt_inst != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_evt_inst);
        s_ip_evt_inst = nullptr;
    }
    (void)esp_wifi_stop();
    (void)esp_wifi_deinit();
    if (s_netif_owned && s_netif != nullptr) {
        esp_netif_destroy(s_netif);
        s_netif = nullptr;
        s_netif_owned = false;
    } else {
        // 复用的 netif 不销毁；指针清空以免悬空误用
        s_netif = nullptr;
        s_netif_owned = false;
    }
}

// 初始化本页 STA 栈。若 WifiStation 已在跑则先 Stop（卸掉其 SCAN_DONE 抢结果），
// 再用偏省 DRAM 的 buffer 配置重新 init。返回是否成功。
bool wifi_init_for_screen() {
    s_wifi_taken_over = true;
    if (s_wifi_initialized) return true;

    // 判断当前 wifi 栈是否已经在跑。esp_wifi_get_mode 在未初始化时会返回
    // ESP_ERR_WIFI_NOT_INIT —— 那种情况下我们不能调用 WifiStation::Stop()
    // （它内部走 ESP_ERROR_CHECK(esp_wifi_stop) 会触发 abort）。
    wifi_mode_t mode_before = WIFI_MODE_NULL;
    esp_err_t mode_err = esp_wifi_get_mode(&mode_before);
    s_wifi_station_was_active = (mode_err == ESP_OK && mode_before != WIFI_MODE_NULL);
    if (WifiManager::GetInstance().IsInitialized()) {
        WifiManager::GetInstance().Deinitialize();
    } else {
        // 上次 init 失败可能留下半吊子驱动状态，先尽量清干净再重试
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
    }

    if (s_evt_group == nullptr) {
        s_evt_group = xEventGroupCreate();
    } else {
        xEventGroupClearBits(s_evt_group, 0xFFFFFF);
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %d", err);
        return false;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %d", err);
        return false;
    }

    // 绝不能二次 create_default_wifi_sta：IDF 在 duplicate if_key 时会 assert 崩溃。
    s_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    s_netif_owned = false;
    if (s_netif == nullptr) {
        s_netif = esp_netif_create_default_wifi_sta();
        if (s_netif == nullptr) {
            ESP_LOGE(TAG, "create wifi netif failed");
            return false;
        }
        s_netif_owned = true;
    }

    // 内部 DRAM 紧张时，默认 16×1600 static RX 容易 malloc fail。
    // 扫描/连网降到较少缓冲即可；离开页面后 WifiStation::Start 会按默认再起。
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    if (cfg.static_rx_buf_num > 6) {
        cfg.static_rx_buf_num = 6;
    }
    if (cfg.static_tx_buf_num > 8) {
        cfg.static_tx_buf_num = 8;
    }
    if (cfg.cache_tx_buf_num > 16) {
        cfg.cache_tx_buf_num = 16;
    }
    if (cfg.rx_mgmt_buf_num > 5) {
        cfg.rx_mgmt_buf_num = 5;
    }
    if (cfg.mgmt_sbuf_num > 16) {
        cfg.mgmt_sbuf_num = 16;
    }

    const size_t largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_LOGI(TAG, "wifi_init: largest DMA block=%uKB", (unsigned)(largest / 1024));

    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %d (DRAM free=%uKB largest=%uKB)",
                 err,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(largest / 1024));
        wifi_init_cleanup_partial();
        return false;
    }

    err = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt_handler, nullptr, &s_wifi_evt_inst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi handler register failed: %d", err);
        wifi_init_cleanup_partial();
        return false;
    }
    err = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_evt_handler, nullptr, &s_ip_evt_inst);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ip handler register failed: %d", err);
        wifi_init_cleanup_partial();
        return false;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %d", err);
        wifi_init_cleanup_partial();
        return false;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %d", err);
        wifi_init_cleanup_partial();
        return false;
    }

    s_wifi_initialized = true;
    ESP_LOGI(TAG, "wifi stack initialized for screen (lean buffers)");
    return true;
}

void wifi_teardown_for_screen() {
    // Viewing saved networks does not initialize a private driver. Do not stop
    // WifiManager's live driver or destroy its netif on a view-only exit.
    if (!s_wifi_taken_over) return;

    if (s_wifi_initialized) {
        esp_wifi_scan_stop();
        esp_wifi_disconnect();

        if (s_wifi_evt_inst != nullptr) {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_evt_inst);
            s_wifi_evt_inst = nullptr;
        }
        if (s_ip_evt_inst != nullptr) {
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_evt_inst);
            s_ip_evt_inst = nullptr;
        }
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
        s_wifi_initialized = false;
        ESP_LOGI(TAG, "wifi stack torn down");
    } else {
        // 失败半途也可能挂着 handler / 半初始化驱动
        if (s_wifi_evt_inst != nullptr) {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_evt_inst);
            s_wifi_evt_inst = nullptr;
        }
        if (s_ip_evt_inst != nullptr) {
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_evt_inst);
            s_ip_evt_inst = nullptr;
        }
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
    }

    // WifiStation::Start() 会再次 create_default_wifi_sta，必须先拆掉 WIFI_STA_DEF
    if (s_netif != nullptr) {
        esp_netif_destroy(s_netif);
        s_netif = nullptr;
    } else {
        esp_netif_t* leftover = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (leftover != nullptr) {
            esp_netif_destroy(leftover);
        }
    }
    s_netif_owned = false;

    // 进入前停过 WifiStation：无论本页 init 是否成功，离开都要恢复
    if (s_wifi_station_was_active) {
        WifiManager::GetInstance().Initialize();
        WifiManager::GetInstance().StartStation();
    }
    s_wifi_station_was_active = false;
    s_wifi_taken_over = false;
}

// ---------------------------------------------------------------------------
// 扫描任务
// ---------------------------------------------------------------------------
void scan_task(void* /*arg*/) {
    post_scan_btn_enabled(false);
    post_nearby_spinner(true);
    // 扫描开始：触发一次列表重建，让 rebuild_nearby_list_now() 看到
    // s_scan_in_progress=true 后把「未发现网络…」那行旧提示清掉，
    // 只留 spinner 单独转。
    refresh_nearby_list();

    // 首次进入页面时 wifi 栈还没初始化，放到 task 内部完成，避免阻塞 LVGL
    // 线程导致 "点开 app 卡几秒才看到页面" 的体验问题。
    if (!s_wifi_initialized) {
        post_status(I18n::T("正在初始化 WiFi…"), kColorScanning);
        if (!wifi_init_for_screen()) {
            post_status(I18n::T("内存不足，WiFi 暂不可用"), kColorError);
            s_scan_in_progress = false;
            refresh_nearby_list();      // 把「未发现网络…」提示画回来
            post_nearby_spinner(false);
            post_scan_btn_enabled(true);
            vTaskDelete(nullptr);
            return;
        }
    }

    post_status(I18n::T("正在扫描附近 WiFi…"), kColorScanning);

    xEventGroupClearBits(s_evt_group, kBitScanDone);

    wifi_scan_config_t cfg = {};
    cfg.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start failed: %d", err);
        char buf[64];
        snprintf(buf, sizeof(buf), I18n::T("启动扫描失败 (err=%d)"), err);
        post_status(buf, kColorError);
        s_scan_in_progress = false;
        refresh_nearby_list();
        post_nearby_spinner(false);
        post_scan_btn_enabled(true);
        vTaskDelete(nullptr);
        return;
    }

    auto bits = xEventGroupWaitBits(s_evt_group, kBitScanDone, pdTRUE, pdTRUE,
                                    pdMS_TO_TICKS(15000));
    if (!(bits & kBitScanDone)) {
        post_status(I18n::T("扫描超时"), kColorError);
        s_scan_in_progress = false;
        refresh_nearby_list();
        post_nearby_spinner(false);
        post_scan_btn_enabled(true);
        vTaskDelete(nullptr);
        return;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);

    std::vector<wifi_ap_record_t> records;
    if (ap_num > 0) {
        records.resize(ap_num);
        uint16_t got = ap_num;
        esp_wifi_scan_get_ap_records(&got, records.data());
        records.resize(got);
    }

    // 排序：RSSI 降序
    std::sort(records.begin(), records.end(),
              [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
                  return a.rssi > b.rssi;
              });

    // 去重（同名只保留信号最强的）
    s_scan_results.clear();
    s_scan_results.reserve(records.size());
    for (auto& r : records) {
        const char* ssid = reinterpret_cast<const char*>(r.ssid);
        if (ssid[0] == '\0') continue;
        bool dup = false;
        for (auto& existing : s_scan_results) {
            if (existing.ssid == ssid) { dup = true; break; }
        }
        if (dup) continue;
        ApItem it;
        it.ssid = ssid;
        it.rssi = r.rssi;
        it.authmode = r.authmode;
        s_scan_results.push_back(std::move(it));
    }

    char status[64];
    snprintf(status, sizeof(status), I18n::T("扫描完成，共 %d 个网络"),
             static_cast<int>(s_scan_results.size()));
    post_status(status, kColorSuccess);

    // 关键顺序：先把 s_scan_in_progress 置 false，再 refresh_nearby_list()。
    // refresh_nearby_list 是 async，跑到 LVGL 那边时如果 flag 还是 true，
    // 在 0 结果场景下会因「扫描中不画提示」而留下一片空白。
    s_scan_in_progress = false;
    refresh_nearby_list();
    post_nearby_spinner(false);
    post_scan_btn_enabled(true);
    vTaskDelete(nullptr);
}

void schedule_scan() {
    if (s_scan_in_progress) {
        ESP_LOGW(TAG, "scan already in progress");
        return;
    }
    if (s_connect_in_progress) {
        post_status(I18n::T("正在连接，请稍后再扫描"), kColorScanning);
        return;
    }
    s_scan_in_progress = true;
    if (xTaskCreate(scan_task, "wifi_scan", 4096, nullptr, 5, nullptr) != pdPASS) {
        s_scan_in_progress = false;
        post_status(I18n::T("无法启动扫描任务"), kColorError);
    }
}

// ---------------------------------------------------------------------------
// 连接任务
// ---------------------------------------------------------------------------
struct ConnectCtx {
    std::string ssid;
    std::string password;
};

void connect_task(void* arg) {
    auto* ctx = static_cast<ConnectCtx*>(arg);

    char buf[128];
    snprintf(buf, sizeof(buf), I18n::T("正在连接 %s …"), ctx->ssid.c_str());
    post_status(buf, kColorScanning);

    // Saved networks can be connected before any scan has initialized this page's driver.
    if (!wifi_init_for_screen()) {
        post_status(I18n::T("内存不足，WiFi 暂不可用"), kColorError);
        post_show_failure(I18n::T("连接失败"), I18n::T("内存不足，WiFi 暂不可用"));
        s_connect_in_progress = false;
        delete ctx;
        vTaskDelete(nullptr);
        return;
    }

    // 准备：停止扫描 + 断开当前连接，然后写新配置 + connect
    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_config_t wc = {};
    std::memcpy(wc.sta.ssid, ctx->ssid.data(), ctx->ssid.size());
    std::memcpy(wc.sta.password, ctx->password.data(), ctx->password.size());
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.failure_retry_cnt = 1;

    xEventGroupClearBits(s_evt_group, kBitConnected | kBitDisconnected);
    s_last_disconnect_reason = 0;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        snprintf(buf, sizeof(buf), I18n::T("set_config 失败 (err=%d)"), err);
        post_status(buf, kColorError);
        post_show_failure(I18n::T("连接失败"), buf);
        s_connect_in_progress = false;
        delete ctx;
        vTaskDelete(nullptr);
        return;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        snprintf(buf, sizeof(buf), I18n::T("esp_wifi_connect 失败 (err=%d)"), err);
        post_status(buf, kColorError);
        post_show_failure(I18n::T("连接失败"), buf);
        s_connect_in_progress = false;
        delete ctx;
        vTaskDelete(nullptr);
        return;
    }

    // 等待 15 秒看是否拿到 IP 或被断开
    auto bits = xEventGroupWaitBits(s_evt_group,
                                    kBitConnected | kBitDisconnected, pdTRUE,
                                    pdFALSE, pdMS_TO_TICKS(15000));

    if (bits & kBitConnected) {
        // 拿到 IP，记一笔到 SsidManager（SaveToNvs 内已经持久化，重启后生效）
        SsidManager::GetInstance().AddSsid(ctx->ssid, ctx->password);
        const auto& saved = SsidManager::GetInstance().GetSsidList();
        for (size_t i = 1; i < saved.size(); ++i) {
            if (saved[i].ssid == ctx->ssid) {
                SsidManager::GetInstance().SetDefaultSsid(static_cast<int>(i));
                break;
            }
        }
        snprintf(buf, sizeof(buf), I18n::T("连接 %s 成功，准备重启…"), ctx->ssid.c_str());
        post_status(buf, kColorSuccess);
        refresh_saved_list();
        // 弹出「连接成功 + 倒计时重启」覆盖层，倒计时归零后自动 esp_restart()
        post_open_success_and_reboot(ctx->ssid);
    } else if (bits & kBitDisconnected) {
        const uint8_t reason = s_last_disconnect_reason;
        const char* mapped = disconnect_reason_text(reason);
        std::string detail;
        if (mapped != nullptr) {
            detail = mapped;
        } else {
            char tmp[96];
            snprintf(tmp, sizeof(tmp), I18n::T("连接被拒绝 (reason=%u)"), reason);
            detail = tmp;
        }
        snprintf(buf, sizeof(buf), I18n::T("连接 %s 失败：%s"),
                 ctx->ssid.c_str(), detail.c_str());
        post_status(buf, kColorError);
        // 失败弹窗保留 2.5 秒让用户看清原因，然后自动收起回到密码键盘
        post_show_failure(I18n::T("连接失败"), detail);
    } else {
        snprintf(buf, sizeof(buf), I18n::T("连接 %s 超时"), ctx->ssid.c_str());
        post_status(buf, kColorError);
        esp_wifi_disconnect();
        post_show_failure(I18n::T("连接超时"), I18n::T("未能在 15 秒内完成连接，请检查网络后重试"));
    }

    s_connect_in_progress = false;
    delete ctx;
    vTaskDelete(nullptr);
}

void schedule_connect(const std::string& ssid, const std::string& password) {
    if (s_scan_in_progress) {
        show_failure_in_status_popup(I18n::T("连接失败"),
                                    I18n::T("请等待扫描结束后再连接"));
        return;
    }
    if (s_connect_in_progress) {
        post_status(I18n::T("已有正在进行的连接任务"), kColorScanning);
        return;
    }
    if (ssid.empty() || ssid.size() > kMaxSsidLen) {
        post_status(I18n::T("SSID 不合法"), kColorError);
        return;
    }
    if (password.size() > kMaxPasswordLen) {
        post_status(I18n::T("密码超长"), kColorError);
        return;
    }
    auto* ctx = new ConnectCtx{ssid, password};
    s_connect_in_progress = true;
    // 立刻弹出转圈遮罩，覆盖密码键盘，让用户得到「我点了连接」的视觉反馈。
    open_connecting_popup(ssid);
    if (xTaskCreate(connect_task, "wifi_connect", 4096, ctx, 5, nullptr) != pdPASS) {
        delete ctx;
        s_connect_in_progress = false;
        post_status(I18n::T("无法启动连接任务"), kColorError);
        close_status_popup();
    }
}

// ---------------------------------------------------------------------------
// 列表渲染
// ---------------------------------------------------------------------------
struct NearbyClickCtx {
    char ssid[kMaxSsidLen + 1];
    int authmode;  // wifi_auth_mode_t
};

void on_nearby_item_clicked(lv_event_t* e) {
    auto* ctx = static_cast<NearbyClickCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;
    open_password_popup(ctx->ssid, static_cast<wifi_auth_mode_t>(ctx->authmode));
}

void on_nearby_item_delete(lv_event_t* e) {
    auto* ctx = static_cast<NearbyClickCtx*>(lv_event_get_user_data(e));
    delete ctx;
}

void rebuild_nearby_list_now() {
    if (s_ui.nearby_list == nullptr) return;
    // 不能用 lv_obj_clean()：会把 nearby_spinner 这个浮层子物件也一起删掉。
    // 改成倒序遍历，只删非 spinner 的旧 item / 提示文本。
    for (int32_t i = static_cast<int32_t>(
                         lv_obj_get_child_count(s_ui.nearby_list)) - 1;
         i >= 0; --i) {
        lv_obj_t* child = lv_obj_get_child(s_ui.nearby_list, i);
        if (child != nullptr && child != s_ui.nearby_spinner) {
            lv_obj_delete(child);
        }
    }

    if (s_scan_results.empty()) {
        // 扫描中（spinner 在转）就别再叠一句「未发现网络…」了，那句话只在
        // 真正扫完发现空结果 / 还没开过扫描的时候才有意义。
        if (s_scan_in_progress) {
            return;
        }
        lv_obj_t* hint = lv_label_create(s_ui.nearby_list);
        lv_label_set_text(hint, I18n::T("未发现网络，点右上「重扫」试试"));
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(hint, &font_puhui_16_4, LV_PART_MAIN);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_pad_all(hint, 12, LV_PART_MAIN);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        return;
    }

    for (auto& ap : s_scan_results) {
        lv_obj_t* item = lv_button_create(s_ui.nearby_list);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_height(item, 40);
        lv_obj_set_style_radius(item, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_color(item, lv_color_hex(kColorItem), LV_PART_MAIN);
        lv_obj_set_style_bg_color(item, lv_color_hex(kColorItemSel),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(item, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(item, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_column(item, 8, LV_PART_MAIN);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_shadow_width(item, 0, LV_PART_MAIN);

        auto* ctx = new NearbyClickCtx{};
        snprintf(ctx->ssid, sizeof(ctx->ssid), "%s", ap.ssid.c_str());
        ctx->authmode = static_cast<int>(ap.authmode);
        lv_obj_add_event_cb(item, on_nearby_item_clicked, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(item, on_nearby_item_delete, LV_EVENT_DELETE, ctx);

        lv_obj_t* left = lv_label_create(item);
        char ltext[96];
        snprintf(ltext, sizeof(ltext), "%s %s", auth_label(ap.authmode),
                 ap.ssid.c_str());
        lv_label_set_text(left, ltext);
        lv_label_set_long_mode(left, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_duration(
            left, lv_anim_speed_clamped(24, 3000, 60000), LV_PART_MAIN);
        lv_obj_set_width(left, 0);
        lv_obj_set_flex_grow(left, 1);
        lv_obj_set_style_text_color(left, lv_color_hex(kColorText), LV_PART_MAIN);
        lv_obj_set_style_text_font(left, &font_puhui_16_4, LV_PART_MAIN);

        lv_obj_t* right = lv_label_create(item);
        char rtext[32];
        snprintf(rtext, sizeof(rtext), "%s", rssi_quality_text(ap.rssi));
        lv_label_set_text(right, rtext);
        lv_obj_set_style_text_color(right, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(right, &font_puhui_16_4, LV_PART_MAIN);
        lv_obj_set_width(right, LV_SIZE_CONTENT);
    }
}

// 已保存 WiFi：主页显示数量，从“查看”进入逐条管理。
void on_saved_view_clicked(lv_event_t* /*e*/) { open_saved_sheet(); }

void update_saved_count_now() {
    if (s_ui.saved_count_lbl == nullptr) {
        return;
    }
    const size_t n = SsidManager::GetInstance().GetSsidList().size();
    char buf[48];
    snprintf(buf, sizeof(buf), I18n::T("已保存 %u 个网络"),
             static_cast<unsigned>(n));
    lv_label_set_text(s_ui.saved_count_lbl, buf);
    if (s_ui.saved_view_btn != nullptr) {
        if (n == 0) {
            lv_obj_add_state(s_ui.saved_view_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(s_ui.saved_view_btn, LV_STATE_DISABLED);
        }
    }
}

struct SavedClickCtx {
    std::string ssid;
};

void on_saved_connect_clicked(lv_event_t* e) {
    const auto* ctx = static_cast<SavedClickCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;
    for (const auto& item : SsidManager::GetInstance().GetSsidList()) {
        if (item.ssid == ctx->ssid) {
            schedule_connect(item.ssid, item.password);
            return;
        }
    }
    refresh_saved_list();
}

void on_saved_remove_clicked(lv_event_t* e) {
    const auto* ctx = static_cast<SavedClickCtx*>(lv_event_get_user_data(e));
    if (ctx == nullptr || s_connect_in_progress) return;
    if (SsidManager::GetInstance().RemoveSsid(ctx->ssid)) {
        post_status(I18n::T("已删除该网络"), kColorSuccess);
    }
    // Defer rebuilding until the clicked button's event has completed.
    refresh_saved_list();
}

void on_saved_card_delete(lv_event_t* e) {
    delete static_cast<SavedClickCtx*>(lv_event_get_user_data(e));
}

void rebuild_saved_list_now() {
    update_saved_count_now();
    if (s_ui.saved_list == nullptr) return;
    lv_obj_clean(s_ui.saved_list);
    const auto& saved = SsidManager::GetInstance().GetSsidList();
    if (saved.empty()) {
        lv_obj_t* hint = lv_label_create(s_ui.saved_list);
        lv_label_set_text(hint, I18n::T("暂无已连接过的 WiFi"));
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_text_font(hint, &font_puhui_16_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        return;
    }
    for (const auto& entry : saved) {
        lv_obj_t* card = lv_obj_create(s_ui.saved_list);
        screen_strip_obj_chrome(card);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_hex(kColorItem), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        auto* ctx = new SavedClickCtx{entry.ssid};
        lv_obj_add_event_cb(card, on_saved_card_delete, LV_EVENT_DELETE, ctx);

        lv_obj_t* name = lv_label_create(card);
        lv_label_set_text(name, entry.ssid.c_str());
        lv_obj_set_width(name, LV_PCT(100));
        lv_label_set_long_mode(name, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_anim_duration(
            name, lv_anim_speed_clamped(24, 3000, 60000), LV_PART_MAIN);
        lv_obj_set_style_text_font(name, &font_puhui_16_4, LV_PART_MAIN);
        lv_obj_set_style_text_color(name, lv_color_hex(kColorText), LV_PART_MAIN);

        lv_obj_t* actions = lv_obj_create(card);
        screen_strip_obj_chrome(actions);
        lv_obj_set_size(actions, LV_PCT(100), 36);
        lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_pad_column(actions, 8, LV_PART_MAIN);
        lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
        lv_obj_remove_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
        auto make_action = [&](const char* text, uint32_t color, lv_event_cb_t callback) {
            lv_obj_t* btn = lv_button_create(actions);
            lv_obj_set_size(btn, 0, 36);
            lv_obj_set_flex_grow(btn, 1);
            lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
            lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
            lv_obj_set_style_bg_color(btn, lv_color_hex(color), LV_PART_MAIN);
            lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
            lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, ctx);
            lv_obj_t* label = lv_label_create(btn);
            lv_label_set_text(label, text);
            lv_obj_set_style_text_font(label, &font_puhui_16_4, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, lv_color_hex(kColorText), LV_PART_MAIN);
            lv_obj_center(label);
        };
        make_action(I18n::T("连接"), kColorBtnActive, on_saved_connect_clicked);
        make_action(I18n::T("删除"), kColorBtnDanger, on_saved_remove_clicked);
    }
}

void close_saved_sheet() {
    if (s_ui.status_overlay != nullptr) return;
    if (s_ui.saved_sheet != nullptr) lv_obj_delete(s_ui.saved_sheet);
    s_ui.saved_sheet = nullptr;
    s_ui.saved_list = nullptr;
}

void open_saved_sheet() {
    if (!screen_alive()) return;
    close_scan_sheet();
    if (s_ui.saved_sheet != nullptr) {
        lv_obj_move_foreground(s_ui.saved_sheet);
        rebuild_saved_list_now();
        return;
    }
    lv_obj_t* sheet = lv_obj_create(s_ui.screen);
    s_ui.saved_sheet = sheet;
    screen_strip_obj_chrome(sheet);
    lv_obj_set_size(sheet, kPanelW, kPanelH);
    lv_obj_set_pos(sheet, 0, 0);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(sheet, true);

    lv_obj_t* header = lv_obj_create(sheet);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    screen_attach_swipe_back(header, []() { close_saved_sheet(); });
    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("已保存 WiFi"));
    lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t* back = lv_button_create(header);
    lv_obj_set_size(back, 56, 26);
    lv_obj_set_style_pad_all(back, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(kColorBtn), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_align(back, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_event_cb(back, [](lv_event_t*) { close_saved_sheet(); }, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(back, true);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, I18n::T("返回"));
    lv_obj_set_style_text_font(back_label, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(back_label, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_center(back_label);

    lv_obj_t* list = lv_obj_create(sheet);
    s_ui.saved_list = list;
    screen_strip_obj_chrome(list);
    lv_obj_set_size(list, kPanelW, kPanelH - kHeaderH);
    lv_obj_set_pos(list, 0, kHeaderH);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    rebuild_saved_list_now();
}

void close_scan_sheet() {
    close_password_popup();
    if (s_ui.scan_sheet != nullptr) {
        lv_obj_delete(s_ui.scan_sheet);
    }
    s_ui.scan_sheet       = nullptr;
    s_ui.sheet_title_lbl  = nullptr;
    s_ui.sheet_rescan_btn = nullptr;
    s_ui.sheet_rescan_lbl = nullptr;
    s_ui.nearby_list      = nullptr;
    s_ui.nearby_spinner   = nullptr;
}

void on_sheet_rescan_clicked(lv_event_t* /*e*/) { schedule_scan(); }

void open_scan_sheet() {
    if (s_ui.screen == nullptr) {
        return;
    }
    close_saved_sheet();
    if (s_ui.scan_sheet != nullptr) {
        lv_obj_move_foreground(s_ui.scan_sheet);
        rebuild_nearby_list_now();
        return;
    }

    lv_obj_t* sheet = lv_obj_create(s_ui.screen);
    s_ui.scan_sheet = sheet;
    screen_strip_obj_chrome(sheet);
    lv_obj_set_size(sheet, kPanelW, kPanelH);
    lv_obj_set_pos(sheet, 0, 0);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(sheet, true);

    lv_obj_t* hd = lv_obj_create(sheet);
    screen_strip_obj_chrome(hd);
    lv_obj_set_size(hd, kPanelW, kHeaderH);
    lv_obj_set_pos(hd, 0, 0);
    lv_obj_set_style_bg_opa(hd, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(hd, LV_OBJ_FLAG_SCROLLABLE);
    // The header remains a swipe-to-close area after removing the back button.
    screen_attach_swipe_back(hd, []() { close_scan_sheet(); });

    lv_obj_t* title = lv_label_create(hd);
    s_ui.sheet_title_lbl = title;
    lv_label_set_text(title,
                      s_scan_in_progress ? I18n::T("扫描中")
                                         : I18n::T("附近 Wi-Fi"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* rescan = lv_button_create(hd);
    s_ui.sheet_rescan_btn = rescan;
    lv_obj_set_size(rescan, 56, 26);
    lv_obj_set_style_pad_all(rescan, 0, LV_PART_MAIN);
    lv_obj_align(rescan, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_radius(rescan, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rescan, lv_color_hex(kColorBtn), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(rescan, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(rescan, on_sheet_rescan_clicked, LV_EVENT_CLICKED, nullptr);
    screen_swipe_back_ignore(rescan, true);
    lv_obj_t* rescan_lbl = lv_label_create(rescan);
    s_ui.sheet_rescan_lbl = rescan_lbl;
    lv_label_set_text(rescan_lbl,
                      s_scan_in_progress ? I18n::T("扫描中") : I18n::T("重扫"));
    lv_obj_set_style_text_color(rescan_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(rescan_lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_center(rescan_lbl);
    if (s_scan_in_progress) {
        lv_obj_add_state(rescan, LV_STATE_DISABLED);
    }

    lv_obj_t* body = lv_obj_create(sheet);
    s_ui.nearby_list = body;
    screen_strip_obj_chrome(body);
    lv_obj_set_size(body, kPanelW, kPanelH - kHeaderH);
    lv_obj_set_pos(body, 0, kHeaderH);
    lv_obj_set_style_bg_color(body, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(body, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
    screen_swipe_back_ignore(body, true);

    lv_obj_t* spin = lv_spinner_create(body);
    lv_obj_set_size(spin, 48, 48);
    lv_obj_add_flag(spin, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(spin, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_arc_color(spin, lv_color_hex(kColorBtnActive),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 4, LV_PART_INDICATOR);
    if (s_scan_in_progress) {
        lv_obj_remove_flag(spin, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(spin, LV_OBJ_FLAG_HIDDEN);
    }
    s_ui.nearby_spinner = spin;

    rebuild_nearby_list_now();
}

// ---------------------------------------------------------------------------
// 密码输入弹窗（模态遮罩 + 文本框 + 键盘）
// ---------------------------------------------------------------------------
// lv_keyboard_create 内部已经把默认 VALUE_CHANGED 回调挂上去（负责把按键
// 回填进 textarea、切换大小写 / 数字键盘 / 关闭等）。这里只补一下用户行为：
//   - LV_EVENT_READY  : 用户按 OK，按当前 textarea 内容发起连接
//   - LV_EVENT_CANCEL : 用户按键盘的关闭按钮，等同于点取消
void on_kb_event(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* kb = lv_event_get_target_obj(e);

    if (code == LV_EVENT_CANCEL) {
        close_password_popup();
    } else if (code == LV_EVENT_READY) {
        lv_obj_t* ta = lv_keyboard_get_textarea(kb);
        const char* pwd = (ta != nullptr) ? lv_textarea_get_text(ta) : "";
        // 开放网络若用户没输入密码也允许（password 留空）
        schedule_connect(s_pending_ssid, pwd ? pwd : "");
    }
}

void on_show_pwd_changed(lv_event_t* e) {
    lv_obj_t* chk = lv_event_get_target_obj(e);
    if (s_ui.pwd_textarea == nullptr) return;
    const bool checked = lv_obj_has_state(chk, LV_STATE_CHECKED);
    lv_textarea_set_password_mode(s_ui.pwd_textarea, !checked);
}

void on_pwd_connect_btn(lv_event_t* /*e*/) {
    if (s_ui.pwd_textarea == nullptr) return;
    const char* pwd = lv_textarea_get_text(s_ui.pwd_textarea);
    schedule_connect(s_pending_ssid, pwd ? pwd : "");
}

void on_pwd_cancel_btn(lv_event_t* /*e*/) { close_password_popup(); }

void open_password_popup(const std::string& ssid, wifi_auth_mode_t authmode) {
    if (s_ui.screen == nullptr) return;
    close_password_popup();

    s_pending_ssid = ssid;
    s_pending_authmode = authmode;

    constexpr int32_t kKbH = 110;
    constexpr int32_t kTopH = kPanelH - kKbH;

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.pwd_overlay = mask;

    lv_obj_t* card = lv_obj_create(mask);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, kPanelW, kTopH);
    lv_obj_set_pos(card, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(card);
    s_ui.pwd_title = title;
    char ttext[96];
    snprintf(ttext, sizeof(ttext), I18n::T("连接 %s"), ssid.c_str());
    lv_label_set_text(title, ttext);
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_16_4, LV_PART_MAIN);
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(title, kPanelW - 16);
    lv_obj_set_height(title, 20);

    lv_obj_t* hint = lv_label_create(card);
    lv_label_set_text(hint,
                      authmode == WIFI_AUTH_OPEN
                          ? I18n::T("开放网络，可直接连接")
                          : I18n::T("请输入 WiFi 密码"));
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_width(hint, kPanelW - 16);
    if (authmode != WIFI_AUTH_OPEN) lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* ta = lv_textarea_create(card);
    s_ui.pwd_textarea = ta;
    lv_obj_set_size(ta, kPanelW - 16, 32);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_max_length(ta, kMaxPasswordLen);
    lv_textarea_set_placeholder_text(ta, I18n::T("WiFi 密码"));
    lv_obj_set_style_text_font(ta, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x121726), LV_PART_MAIN);
    lv_obj_set_style_text_color(ta, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_radius(ta, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(ta, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(ta, 6, LV_PART_MAIN);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    screen_swipe_back_ignore(ta, true);
    if (authmode == WIFI_AUTH_OPEN) {
        lv_obj_add_flag(ta, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* chk = lv_checkbox_create(card);
    s_ui.pwd_show_chk = chk;
    lv_checkbox_set_text(chk, I18n::T("显示密码"));
    lv_obj_set_height(chk, 24);
    lv_obj_set_style_pad_all(chk, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chk, 0, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(chk, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(chk, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_add_event_cb(chk, on_show_pwd_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    screen_swipe_back_ignore(chk, true);
    if (authmode == WIFI_AUTH_OPEN) {
        lv_obj_add_flag(chk, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* btn_row = lv_obj_create(card);
    screen_strip_obj_chrome(btn_row);
    lv_obj_set_size(btn_row, kPanelW - 16, 30);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, 8, LV_PART_MAIN);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* cancel = lv_button_create(btn_row);
    lv_obj_set_size(cancel, 0, 30);
    lv_obj_set_style_pad_all(cancel, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_style_radius(cancel, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(kColorBtn), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cancel, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel, on_pwd_cancel_btn, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, I18n::T("取消"));
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(cancel_lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_center(cancel_lbl);
    screen_swipe_back_ignore(cancel, true);

    lv_obj_t* connect = lv_button_create(btn_row);
    lv_obj_set_size(connect, 0, 30);
    lv_obj_set_style_pad_all(connect, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(connect, 1);
    lv_obj_set_style_radius(connect, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(connect, lv_color_hex(kColorBtnActive), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(connect, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(connect, on_pwd_connect_btn, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* connect_lbl = lv_label_create(connect);
    lv_label_set_text(connect_lbl, I18n::T("连接"));
    lv_obj_set_style_text_color(connect_lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(connect_lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_center(connect_lbl);
    screen_swipe_back_ignore(connect, true);

    lv_obj_t* kb = lv_keyboard_create(mask);
    s_ui.pwd_keyboard = kb;
    lv_obj_set_size(kb, kPanelW, kKbH);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(kb, on_kb_event, LV_EVENT_CANCEL, nullptr);
    screen_swipe_back_ignore(kb, true);
    if (authmode == WIFI_AUTH_OPEN) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(card, kPanelH);
    }
}

void close_password_popup() {
    if (s_ui.pwd_overlay != nullptr) {
        lv_obj_delete(s_ui.pwd_overlay);
    }
    s_ui.pwd_overlay  = nullptr;
    s_ui.pwd_textarea = nullptr;
    s_ui.pwd_keyboard = nullptr;
    s_ui.pwd_title    = nullptr;
    s_ui.pwd_show_chk = nullptr;
    s_pending_ssid.clear();
}

// ---------------------------------------------------------------------------
// 连接进度 / 成功弹窗
//
// 连接发起后用一个独立的全屏遮罩盖住整个屏幕：
//   - 连接中：居中显示一个转圈 spinner + 状态文本
//   - 成功后：spinner 消失，文本变成「连接成功，N 秒后重启…」并倒计时，
//     倒计时归零后异步触发 esp_restart()
// 失败时调用 close_status_popup() 把这一层撤掉，让用户回到密码弹窗重试。
// ---------------------------------------------------------------------------
void close_status_popup() {
    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
        s_restart_timer = nullptr;
    }
    if (s_ui.status_overlay != nullptr) {
        lv_obj_delete(s_ui.status_overlay);
    }
    s_ui.status_overlay     = nullptr;
    s_ui.status_message_lbl = nullptr;
    s_restart_remaining = 0;
    s_restart_headline.clear();
}

// All status dialogs share a vertical flow; wrapped text never overlaps the heading/spinner.
lv_obj_t* create_status_card(lv_obj_t* overlay) {
    lv_obj_t* card = lv_obj_create(overlay);
    screen_strip_obj_chrome(card);
    lv_obj_set_size(card, 210, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, 100, LV_PART_MAIN);
    lv_obj_set_style_max_height(card, kPanelH - 20, LV_PART_MAIN);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(kColorCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);
    screen_swipe_back_ignore(card, true);
    return card;
}

void open_connecting_popup(const std::string& ssid) {
    if (s_ui.screen == nullptr) return;
    close_status_popup();

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.status_overlay = mask;

    lv_obj_t* card = create_status_card(mask);

    lv_obj_t* spin = lv_spinner_create(card);
    lv_obj_set_size(spin, 36, 36);
    lv_spinner_set_anim_params(spin, 1000, 200);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(kColorBtnActive), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_INDICATOR);

    lv_obj_t* lbl = lv_label_create(card);
    s_ui.status_message_lbl = lbl;
    lv_label_set_text(lbl, I18n::T("正在连接"));
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t* network = lv_label_create(card);
    lv_label_set_text(network, ssid.c_str());
    lv_obj_set_width(network, LV_PCT(100));
    lv_label_set_long_mode(network, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(
        network, lv_anim_speed_clamped(24, 3000, 60000), LV_PART_MAIN);
    lv_obj_set_style_text_font(network, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_text_color(network, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_align(network, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

void restart_timer_cb(lv_timer_t* /*timer*/) {
    s_restart_remaining--;
    if (s_restart_remaining > 0) {
        if (s_ui.status_message_lbl != nullptr) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     I18n::T("%s\n设备将在 %d 秒后自动重启…"),
                     s_restart_headline.c_str(), s_restart_remaining);
            lv_label_set_text(s_ui.status_message_lbl, buf);
        }
        return;
    }

    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
        s_restart_timer = nullptr;
    }
    if (s_ui.status_message_lbl != nullptr) {
        lv_label_set_text(s_ui.status_message_lbl, I18n::T("正在重启…"));
    }
    // MQTT/TLS destruction exceeds the former 2 KB wifi_reboot stack. Run it
    // on the application's 8 KB main task, which already owns protocol state.
    Application::GetInstance().Schedule([] {
        ESP_LOGI(TAG, "network config done -> Application::Reboot()");
        vTaskDelay(pdMS_TO_TICKS(200));
        Application::GetInstance().Reboot();
    });
}

// 失败提示自动关闭：复用 s_restart_timer 槽位，到期回调里只做收掉遮罩，
// 重启逻辑走的是另一个 cb（restart_timer_cb），两者互斥不会同时存在。
// 注意：repeat_count=1 的 lv_timer 触发后会自动 delete，所以这里要先把
// s_restart_timer 置空，避免 close_status_popup 再去 lv_timer_delete 一个野指针。
void failure_close_timer_cb(lv_timer_t* /*timer*/) {
    s_restart_timer = nullptr;
    close_status_popup();
}

// 把当前的 status_overlay 替换成「失败」卡片，N 毫秒后自动关闭遮罩，让用户
// 回到密码键盘重新输入。如果遮罩已经被关掉了（极端情况），就新建一个。
void show_failure_in_status_popup(const std::string& title,
                                  const std::string& detail,
                                  uint32_t auto_close_ms) {
    if (s_ui.screen == nullptr) return;

    // 复用 / 新建遮罩
    if (s_ui.status_overlay == nullptr) {
        lv_obj_t* mask = lv_obj_create(s_ui.screen);
        screen_strip_obj_chrome(mask);
        lv_obj_set_size(mask, kPanelW, kPanelH);
        lv_obj_set_pos(mask, 0, 0);
        lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
        lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
        screen_swipe_back_ignore(mask, true);
        s_ui.status_overlay = mask;
    } else {
        lv_obj_clean(s_ui.status_overlay);
        s_ui.status_message_lbl = nullptr;
    }

    lv_obj_t* card = create_status_card(s_ui.status_overlay);

    lv_obj_t* head = lv_label_create(card);
    lv_label_set_text(head, title.c_str());
    lv_obj_set_style_text_color(head, lv_color_hex(kColorBtnDanger), LV_PART_MAIN);
    lv_obj_set_style_text_font(head, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_width(head, LV_PCT(100));
    lv_label_set_long_mode(head, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t* body = lv_label_create(card);
    s_ui.status_message_lbl = body;
    lv_label_set_text(body, detail.c_str());
    lv_obj_set_width(body, LV_PCT(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
        s_restart_timer = nullptr;
    }
    s_restart_timer = lv_timer_create(failure_close_timer_cb, auto_close_ms, nullptr);
    lv_timer_set_repeat_count(s_restart_timer, 1);
}

void open_restart_countdown_popup(const std::string& headline) {
    if (s_ui.screen == nullptr) return;
    close_status_popup();

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.status_overlay = mask;

    lv_obj_t* card = create_status_card(mask);

    lv_obj_t* check = lv_label_create(card);
    lv_label_set_text(check, I18n::T("成功"));
    lv_obj_set_style_text_color(check, lv_color_hex(kColorBtnAccent), LV_PART_MAIN);
    lv_obj_set_style_text_font(check, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_width(check, LV_PCT(100));
    lv_label_set_long_mode(check, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(check, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    s_restart_headline = headline;
    s_restart_remaining = kRestartCountdownSec;

    lv_obj_t* lbl = lv_label_create(card);
    s_ui.status_message_lbl = lbl;
    char buf[160];
    snprintf(buf, sizeof(buf), I18n::T("%s\n设备将在 %d 秒后自动重启…"),
             s_restart_headline.c_str(), s_restart_remaining);
    lv_label_set_text(lbl, buf);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
    }
    s_restart_timer = lv_timer_create(restart_timer_cb, 1000, nullptr);
}

void open_success_popup(const std::string& ssid) {
    char buf[160];
    snprintf(buf, sizeof(buf), I18n::T("%s 连接成功！"), ssid.c_str());
    open_restart_countdown_popup(buf);
}

// ---------------------------------------------------------------------------
// 头部按钮 / 屏幕生命周期
// ---------------------------------------------------------------------------
void on_scan_clicked(lv_event_t* /*e*/) {
    open_scan_sheet();
    schedule_scan();
}

void on_swipe_back() {
    // Keep the page alive while a worker owns its driver/UI result callbacks.
    if (s_scan_in_progress || s_connect_in_progress ||
        s_network_switch_pending || s_sim_switch_pending) return;

    // 弹窗存在时优先关闭弹窗，不触发返回。
    // 连接 / 成功倒计时遮罩不允许通过滑动关闭——前者要等结果，后者会自动重启。
    if (s_ui.status_overlay != nullptr) {
        return;
    }
    if (s_ui.pwd_overlay != nullptr) {
        close_password_popup();
        return;
    }
    if (s_ui.scan_sheet != nullptr) {
        close_scan_sheet();
        return;
    }
    if (s_ui.saved_sheet != nullptr) {
        close_saved_sheet();
        return;
    }
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) lv_indev_wait_release(indev);
    if (screen_nav_try_return_to_settings()) {
        return;
    }
    lv_obj_t* old_scr = lv_screen_active();
    lv_obj_t* home    = HomeScreen::Create();
    lv_screen_load(home);
    if (old_scr != nullptr && old_scr != home) {
        lv_obj_delete_async(old_scr);
    }
}

void on_screen_unloaded(lv_event_t* /*e*/) {
    s_screen_active = false;
    s_ui.screen        = nullptr;
    s_ui.status_label  = nullptr;
    s_ui.scan_btn      = nullptr;
    s_ui.scan_btn_lbl  = nullptr;
    s_ui.tabview       = nullptr;
    s_ui.nearby_tab    = nullptr;
    s_ui.saved_tab       = nullptr;
    s_ui.network_tab     = nullptr;
    s_ui.sim_tab         = nullptr;
    s_ui.nearby_list     = nullptr;
    s_ui.nearby_spinner  = nullptr;
    s_ui.saved_list      = nullptr;
    s_ui.saved_sheet     = nullptr;
    s_ui.saved_view_btn  = nullptr;
    s_ui.saved_count_lbl = nullptr;
    s_ui.scan_sheet      = nullptr;
    s_ui.sheet_title_lbl = nullptr;
    s_ui.sheet_rescan_btn = nullptr;
    s_ui.sheet_rescan_lbl = nullptr;
    s_ui.network_wifi_btn    = nullptr;
    s_ui.network_wifi_lbl    = nullptr;
    s_ui.network_cell_btn    = nullptr;
    s_ui.network_cell_lbl    = nullptr;
    s_ui.network_current_lbl = nullptr;
    s_ui.sim_external_btn = nullptr;
    s_ui.sim_external_lbl = nullptr;
    s_ui.sim_internal_btn = nullptr;
    s_ui.sim_internal_lbl = nullptr;
    s_ui.sim_current_lbl  = nullptr;
    s_network_switch_pending = false;
    // 注意：s_sim_switch_pending 不在这里清零——AT 任务可能还在后台跑，
    // 它结束后回调里会检测 screen_alive() 并自行复位。
    s_ui.pwd_overlay   = nullptr;
    s_ui.pwd_textarea  = nullptr;
    s_ui.pwd_keyboard  = nullptr;
    s_ui.pwd_title     = nullptr;
    s_ui.pwd_show_chk  = nullptr;
    // 屏幕销毁时 lvgl 会随父对象一起把 overlay 删掉；但 lv_timer 不挂在
    // 对象树上，必须显式 delete，否则到期后会回调到野指针。
    if (s_restart_timer != nullptr) {
        lv_timer_delete(s_restart_timer);
        s_restart_timer = nullptr;
    }
    s_ui.status_overlay     = nullptr;
    s_ui.status_message_lbl = nullptr;
    s_restart_remaining = 0;
    s_restart_headline.clear();
    s_pending_ssid.clear();
    s_scan_results.clear();
}

// ---------------------------------------------------------------------------
// 网络切换（WiFi <-> 4G）
// ---------------------------------------------------------------------------
void switch_network_task(void* /*arg*/) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (auto* dual = GetDualNetworkBoard()) {
        dual->SwitchNetworkType();
    } else {
        ESP_LOGE(TAG, "DualNetworkBoard not available, reboot anyway");
        esp_restart();
    }
    vTaskDelete(nullptr);
}

void open_switch_reboot_popup(const char* target_name) {
    if (s_ui.screen == nullptr) {
        return;
    }
    close_status_popup();

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.status_overlay = mask;

    lv_obj_t* card = create_status_card(mask);

    lv_obj_t* head = lv_label_create(card);
    lv_label_set_text(head, I18n::T("切换网络"));
    lv_obj_set_style_text_color(head, lv_color_hex(kColorBtnActive), LV_PART_MAIN);
    lv_obj_set_style_text_font(head, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_set_width(head, LV_PCT(100));
    lv_label_set_long_mode(head, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t* body = lv_label_create(card);
    s_ui.status_message_lbl = body;
    char buf[160];
    snprintf(buf, sizeof(buf), I18n::T("正在切换到 %s\n设备即将重启…"), target_name);
    lv_label_set_text(body, buf);
    lv_obj_set_width(body, LV_PCT(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    if (xTaskCreate(switch_network_task, "net_switch", 4096, nullptr, 5,
                    nullptr) != pdPASS) {
        s_network_switch_pending = false;
        post_status(I18n::T("无法启动切换任务"), kColorError);
        close_status_popup();
        refresh_network_switch_ui();
    }
}

// 把"按钮 → 切换"逻辑收成一个函数，两个上网方式按钮共用：
//   * 同一目标点两次 / 当前已经是该模式：no-op，避免重复进入重启流程
//   * 没有 4G 板（DualNetworkBoard 不可用）：状态栏报错并把高亮拉回真值
//   * 否则进入重启确认弹窗，由 switch_network_task 异步触发设备重启
void schedule_network_switch(int target_type) {
    if (s_network_switch_pending) {
        return;
    }
    if (target_type != kNetTypeWifi && target_type != kNetTypeCellular) {
        return;
    }
    if (GetSavedNetworkType() == target_type) {
        return;
    }

    if (GetDualNetworkBoard() == nullptr) {
        post_status(I18n::T("当前设备不支持网络切换"), kColorError);
        refresh_network_switch_ui();
        return;
    }

    s_network_switch_pending = true;
    const char* target = target_type == kNetTypeCellular ? "4G" : "WiFi";
    post_status(target_type == kNetTypeCellular ? I18n::T("准备切换到 4G…")
                                                 : I18n::T("准备切换到 WiFi…"),
                kColorScanning);
    open_switch_reboot_popup(target);
}

void on_network_wifi_clicked(lv_event_t* /*e*/) {
    schedule_network_switch(kNetTypeWifi);
}

void on_network_cell_clicked(lv_event_t* /*e*/) {
    schedule_network_switch(kNetTypeCellular);
}

// 把两个上网方式按钮的高亮状态同步成「当前模式高亮、另一个置灰」。
// 行为完全对应 refresh_sim_slot_ui()，方便直观比对维护。
void refresh_network_switch_ui() {
    if (s_ui.network_wifi_btn == nullptr ||
        s_ui.network_cell_btn == nullptr) {
        return;
    }
    const int type = GetSavedNetworkType();
    const bool is_wifi = (type != kNetTypeCellular);

    lv_obj_set_style_bg_color(s_ui.network_wifi_btn,
                              lv_color_hex(is_wifi ? 0x1E3A5F : 0x151922),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.network_wifi_btn,
                                  lv_color_hex(is_wifi ? kColorBtnActive
                                                       : 0x2A2F3A),
                                  LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.network_cell_btn,
                              lv_color_hex(is_wifi ? 0x151922 : 0x1E3A5F),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.network_cell_btn,
                                  lv_color_hex(is_wifi ? 0x2A2F3A
                                                       : kColorBtnActive),
                                  LV_PART_MAIN);
    if (s_ui.network_wifi_lbl != nullptr) {
        lv_obj_set_style_text_color(
            s_ui.network_wifi_lbl,
            lv_color_hex(is_wifi ? kColorText : kColorSubtle), LV_PART_MAIN);
    }
    if (s_ui.network_cell_lbl != nullptr) {
        lv_obj_set_style_text_color(
            s_ui.network_cell_lbl,
            lv_color_hex(is_wifi ? kColorSubtle : kColorText), LV_PART_MAIN);
    }

    if (s_ui.network_current_lbl != nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), I18n::T("当前：%s"), is_wifi ? "WiFi" : "4G");
        lv_label_set_text(s_ui.network_current_lbl, buf);
    }
}

// ---------------------------------------------------------------------------
// SIM 卡切换（仅 4G 模式）
//
// 切换序列（参考 4G 模组手册）：
//   1) AT+CFUN=0           关闭射频，进入最小功能模式
//   2) AT+ECSIMCFG=SimSlot,X   X=0 外置卡 / X=1 内置卡
//   3) AT+CFUN=1           重新打开射频
// 整个过程模组会重新搜网，期间网络短时中断；我们用一个全屏遮罩展示进度。
// 成功之后保存 UI 偏好，并弹出倒计时重启提示（与 WiFi 连接成功一致）。
// ---------------------------------------------------------------------------

// 把两个 SIM 槽位按钮的高亮状态同步成「当前槽位高亮、另一个置灰」。
void refresh_sim_slot_ui() {
    if (s_ui.sim_external_btn == nullptr || s_ui.sim_internal_btn == nullptr) {
        return;
    }
    const int slot = GetSavedSimSlot();
    const bool is_external = (slot == kSimSlotExternal);

    lv_obj_set_style_bg_color(s_ui.sim_external_btn,
                              lv_color_hex(is_external ? kColorBtnActive
                                                       : kColorBtn),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.sim_internal_btn,
                              lv_color_hex(is_external ? kColorBtn
                                                       : kColorBtnActive),
                              LV_PART_MAIN);

    if (s_ui.sim_current_lbl != nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), I18n::T("当前：%s"), SimSlotName(slot));
        lv_label_set_text(s_ui.sim_current_lbl, buf);
    }
}

// 进度文本通过 status_message_lbl 异步刷新（在 LVGL 线程）
void async_sim_set_progress(void* user_data) {
    auto* msg = static_cast<AsyncStringMsg*>(user_data);
    if (screen_alive() && s_ui.status_message_lbl != nullptr) {
        lv_label_set_text(s_ui.status_message_lbl, msg->text.c_str());
    }
    delete msg;
}

void post_sim_progress(const std::string& text) {
    if (!s_screen_active) return;
    lv_async_call(async_sim_set_progress, new AsyncStringMsg{text});
}

// 从 AT+ECSIMCFG? 的响应里解析出 SimSlot 行。完整响应类似：
//   +ECSIMCFG: "SimSimulator",0
//   +ECSIMCFG: "SimSlot",1
//   ...
//   OK
// 返回 -1 表示没解析到（响应不完整 / 没回 OK / 模组没回这一行）。
int parse_sim_slot_from_ecsimcfg(const std::string& resp) {
    constexpr const char* kKey = "\"SimSlot\"";
    size_t pos = 0;
    while ((pos = resp.find(kKey, pos)) != std::string::npos) {
        size_t comma = resp.find(',', pos);
        if (comma == std::string::npos) return -1;
        // 跳过逗号后的空白
        size_t i = comma + 1;
        while (i < resp.size() && (resp[i] == ' ' || resp[i] == '\t')) ++i;
        if (i >= resp.size() || !std::isdigit(static_cast<unsigned char>(resp[i]))) {
            pos = comma + 1;
            continue;
        }
        int slot = 0;
        while (i < resp.size() && std::isdigit(static_cast<unsigned char>(resp[i]))) {
            slot = slot * 10 + (resp[i] - '0');
            ++i;
        }
        return slot;
    }
    return -1;
}

// 把异步查到的槽位投递回 LVGL 线程：写 NVS 并刷新 UI 高亮 / "当前" 文字。
struct SimSlotQueryMsg {
    int slot;  // 0 / 1，-1 表示查询失败（不写 NVS，只刷新一下显示）
};

void async_sim_slot_queried(void* user_data) {
    auto* msg = static_cast<SimSlotQueryMsg*>(user_data);
    if (screen_alive() && msg->slot >= 0) {
        if (GetSavedSimSlot() != msg->slot) {
            SaveSimSlot(msg->slot);
            ESP_LOGI(TAG, "sim_slot synced from modem: %d", msg->slot);
        }
        refresh_sim_slot_ui();
    }
    delete msg;
}

void sim_slot_query_task(void* /*arg*/) {
    auto* msg = new SimSlotQueryMsg{-1};
    lv_async_call(async_sim_slot_queried, msg);
    vTaskDelete(nullptr);
}

void schedule_sim_slot_query() {
    if (!HasDualSimModem()) {
        return;
    }
    if (xTaskCreate(sim_slot_query_task, "sim_query", 4096, nullptr, 5,
                    nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(sim_query) failed");
    }
}

struct SimSwitchResultMsg {
    bool        success;
    int         target_slot;
    std::string detail;   // 失败时的提示语
};

void async_sim_switch_done(void* user_data) {
    auto* msg = static_cast<SimSwitchResultMsg*>(user_data);
    if (screen_alive()) {
        s_sim_switch_pending = false;
        if (msg->success) {
            char buf[96];
            snprintf(buf, sizeof(buf), I18n::T("已切换到%s"),
                     SimSlotName(msg->target_slot));
            open_restart_countdown_popup(buf);
            refresh_sim_slot_ui();
        } else {
            close_status_popup();
            post_status(msg->detail.c_str(), kColorError);
            show_failure_in_status_popup(I18n::T("SIM 卡切换失败"), msg->detail, 3000);
            // 失败也查一次，保证 UI 与模组当前实际状态对齐
            schedule_sim_slot_query();
            refresh_sim_slot_ui();
        }
    } else {
        s_sim_switch_pending = false;
    }
    delete msg;
}

struct SimSwitchCtx {
    int target_slot;
};

void sim_switch_task(void* arg) {
    auto* ctx = static_cast<SimSwitchCtx*>(arg);
    auto* result = new SimSwitchResultMsg{};
    result->target_slot = ctx->target_slot;
    result->success = false;
    result->detail = I18n::T("本机不支持双 SIM 切换");
    lv_async_call(async_sim_switch_done, result);
    delete ctx;
    vTaskDelete(nullptr);
}

// 弹出一个全屏遮罩告诉用户正在切换 SIM 卡。复用 status_overlay 槽位。
void open_sim_switching_popup(int target_slot) {
    if (s_ui.screen == nullptr) return;
    close_status_popup();

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    screen_strip_obj_chrome(mask);
    lv_obj_set_size(mask, kPanelW, kPanelH);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_80, LV_PART_MAIN);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    screen_swipe_back_ignore(mask, true);
    s_ui.status_overlay = mask;

    lv_obj_t* card = create_status_card(mask);

    lv_obj_t* spin = lv_spinner_create(card);
    lv_obj_set_size(spin, 36, 36);
    lv_spinner_set_anim_params(spin, 1000, 200);
    lv_obj_set_style_arc_color(spin, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, lv_color_hex(kColorBtnActive),
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 10, LV_PART_INDICATOR);

    lv_obj_t* lbl = lv_label_create(card);
    s_ui.status_message_lbl = lbl;
    char buf[160];
    snprintf(buf, sizeof(buf), I18n::T("正在切换到%s…\nAT+CFUN=0"),
             SimSlotName(target_slot));
    lv_label_set_text(lbl, buf);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &font_puhui_16_4, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

void schedule_sim_switch(int target_slot) {
    if (s_sim_switch_pending) {
        return;
    }
    if (target_slot != kSimSlotExternal && target_slot != kSimSlotInternal) {
        return;
    }
    if (GetSavedSimSlot() == target_slot) {
        // 同一张卡，无需切换
        return;
    }
    if (!HasDualSimModem()) {
        post_status(I18n::T("当前不在 4G 模式，无法切换 SIM 卡"), kColorError);
        return;
    }

    s_sim_switch_pending = true;
    auto* ctx = new SimSwitchCtx{target_slot};
    open_sim_switching_popup(target_slot);
    if (xTaskCreate(sim_switch_task, "sim_switch", 4096, ctx, 5, nullptr) !=
        pdPASS) {
        delete ctx;
        s_sim_switch_pending = false;
        close_status_popup();
        post_status(I18n::T("无法启动 SIM 切换任务"), kColorError);
    }
}

void on_sim_external_clicked(lv_event_t* /*e*/) {
    schedule_sim_switch(kSimSlotExternal);
}

void on_sim_internal_clicked(lv_event_t* /*e*/) {
    schedule_sim_switch(kSimSlotInternal);
}

// ---------------------------------------------------------------------------
// UI 组装
// ---------------------------------------------------------------------------
void build_header(lv_obj_t* parent) {
    lv_obj_t* header = lv_obj_create(parent);
    screen_strip_obj_chrome(header);
    lv_obj_set_size(header, kPanelW, kHeaderH);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, I18n::T("网络配置"));
    lv_obj_set_style_text_color(title, lv_color_hex(kColorText), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &font_puhui_20_4, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
}

void build_tabview(lv_obj_t* parent) {
    // HTML 风格：顶部分段 Wi-Fi/4G + 同页列表（不再用多 Tab）
    constexpr int y = kHeaderH;
    constexpr int h = kPanelH - y;

    lv_obj_t* body = lv_obj_create(parent);
    s_ui.tabview = body;  // 复用字段：整页内容根
    screen_strip_obj_chrome(body);
    lv_obj_set_size(body, kPanelW, h);
    lv_obj_set_pos(body, 0, y);
    lv_obj_set_style_bg_color(body, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(body, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Wi-Fi | 4G 分段 ----
    lv_obj_t* seg = lv_obj_create(body);
    s_ui.network_tab = seg;
    screen_strip_obj_chrome(seg);
    lv_obj_set_size(seg, LV_PCT(100), 34);
    lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(seg, 6, LV_PART_MAIN);

    auto make_net_btn = [&](const char* text, lv_event_cb_t cb, lv_obj_t** out_btn,
                            lv_obj_t** out_lbl) {
        lv_obj_t* btn = lv_button_create(seg);
        lv_obj_set_height(btn, 34);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x151922), LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x2A2F3A), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        screen_swipe_back_ignore(btn, true);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
        lv_obj_center(lbl);
        *out_btn = btn;
        *out_lbl = lbl;
    };
    make_net_btn("Wi-Fi", on_network_wifi_clicked, &s_ui.network_wifi_btn,
                 &s_ui.network_wifi_lbl);
    make_net_btn("4G", on_network_cell_clicked, &s_ui.network_cell_btn,
                 &s_ui.network_cell_lbl);

    lv_obj_t* hint = lv_label_create(body);
    lv_label_set_text(hint, I18n::T("上网方式 · 切换后设备将重启"));
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(hint, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &font_puhui_16_4, LV_PART_MAIN);

    lv_obj_t* cur = lv_label_create(body);
    s_ui.network_current_lbl = cur;
    lv_label_set_text(cur, I18n::T("当前：--"));
    lv_obj_set_style_text_color(cur, lv_color_hex(kColorSubtle), LV_PART_MAIN);
    lv_obj_set_style_text_font(cur, &font_puhui_16_4, LV_PART_MAIN);

    // ---- 内容区按行排布；状态提示变长时允许纵向滚动 ----
    lv_obj_t* scroll = lv_obj_create(body);
    screen_strip_obj_chrome(scroll);
    lv_obj_set_width(scroll, LV_PCT(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scroll, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(scroll, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    screen_swipe_back_ignore(scroll, true);

    const bool wifi_mode = !IsCellularMode();

    if (wifi_mode) {
        s_ui.nearby_tab = scroll;
        s_ui.saved_tab = scroll;

        lv_obj_t* status = lv_label_create(scroll);
        s_ui.status_label = status;
        lv_label_set_text(status, "");
        lv_obj_set_width(status, LV_PCT(100));
        lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
        lv_obj_add_flag(status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(status, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(status, &font_puhui_16_4, LV_PART_MAIN);

        lv_obj_t* scan = lv_button_create(scroll);
        s_ui.scan_btn = scan;
        lv_obj_set_width(scan, LV_PCT(100));
        lv_obj_set_height(scan, 36);
        lv_obj_set_style_radius(scan, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(scan, lv_color_hex(kColorBtnActive),
                                  LV_PART_MAIN);
        lv_obj_set_style_shadow_width(scan, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(scan, 0, LV_PART_MAIN);
        lv_obj_set_style_clip_corner(scan, false, LV_PART_MAIN);
        lv_obj_add_event_cb(scan, on_scan_clicked, LV_EVENT_CLICKED, nullptr);
        screen_swipe_back_ignore(scan, true);
        lv_obj_t* scan_lbl = lv_label_create(scan);
        s_ui.scan_btn_lbl = scan_lbl;
        lv_label_set_text(scan_lbl, I18n::T("扫描附近网络"));
        lv_obj_set_style_text_color(scan_lbl, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(scan_lbl, &font_puhui_16_4, LV_PART_MAIN);
        lv_obj_center(scan_lbl);

        lv_obj_t* saved_row = lv_obj_create(scroll);
        screen_strip_obj_chrome(saved_row);
        lv_obj_set_size(saved_row, LV_PCT(100), 36);
        lv_obj_set_style_radius(saved_row, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_color(saved_row, lv_color_hex(kColorCard),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(saved_row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(saved_row, 10, LV_PART_MAIN);
        lv_obj_remove_flag(saved_row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* saved_count = lv_label_create(saved_row);
        s_ui.saved_count_lbl = saved_count;
        lv_label_set_text(saved_count, I18n::T("已保存 0 个网络"));
        lv_obj_set_style_text_color(saved_count, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(saved_count, &font_puhui_16_4, LV_PART_MAIN);
        lv_obj_align(saved_count, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t* view = lv_button_create(saved_row);
        s_ui.saved_view_btn = view;
        lv_obj_set_size(view, 48, 28);
        lv_obj_set_style_pad_all(view, 0, LV_PART_MAIN);
        lv_obj_align(view, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(view, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_color(view, lv_color_hex(kColorBtnActive),
                                  LV_PART_MAIN);
        lv_obj_set_style_shadow_width(view, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(view, on_saved_view_clicked, LV_EVENT_CLICKED, nullptr);
        screen_swipe_back_ignore(view, true);
        lv_obj_t* view_lbl = lv_label_create(view);
        lv_label_set_text(view_lbl, I18n::T("查看"));
        lv_obj_set_style_text_color(view_lbl, lv_color_hex(kColorText),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(view_lbl, &font_puhui_16_4, LV_PART_MAIN);
        lv_obj_center(view_lbl);

        lv_obj_t* tip = lv_label_create(scroll);
        lv_label_set_text(tip, I18n::T("扫描结果在弹窗中选择并连接"));
        lv_obj_set_width(tip, LV_PCT(100));
        lv_label_set_long_mode(tip, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(tip, lv_color_hex(kColorSubtle), LV_PART_MAIN);
        lv_obj_set_style_text_font(tip, &font_puhui_16_4, LV_PART_MAIN);

        update_saved_count_now();
    } else {
        // 4G：状态 + SIM
        s_ui.sim_tab = scroll;
        lv_obj_t* status = lv_label_create(scroll);
        s_ui.status_label = status;
        lv_label_set_text(status, "");
        lv_obj_add_flag(status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(status, LV_PCT(100));
        lv_obj_set_style_text_color(status, lv_color_hex(kColorSubtle),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_font(status, &font_puhui_20_4, LV_PART_MAIN);

        if (HasDualSimModem()) {
            lv_obj_t* sim_card = lv_obj_create(scroll);
            screen_strip_obj_chrome(sim_card);
            lv_obj_set_size(sim_card, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(sim_card, lv_color_hex(kColorCard),
                                      LV_PART_MAIN);
            lv_obj_set_style_bg_opa(sim_card, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_radius(sim_card, 10, LV_PART_MAIN);
            lv_obj_set_style_pad_all(sim_card, 8, LV_PART_MAIN);
            lv_obj_set_style_pad_row(sim_card, 6, LV_PART_MAIN);
            lv_obj_set_flex_flow(sim_card, LV_FLEX_FLOW_COLUMN);
            lv_obj_remove_flag(sim_card, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t* sim_title = lv_label_create(sim_card);
            lv_label_set_text(sim_title, I18n::T("SIM 卡"));
            lv_obj_set_style_text_color(sim_title, lv_color_hex(kColorText),
                                        LV_PART_MAIN);
            lv_obj_set_style_text_font(sim_title, &font_puhui_20_4, LV_PART_MAIN);

            lv_obj_t* sim_row = lv_obj_create(sim_card);
            screen_strip_obj_chrome(sim_row);
            lv_obj_set_size(sim_row, LV_PCT(100), 36);
            lv_obj_set_style_bg_opa(sim_row, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_flex_flow(sim_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(sim_row, 6, LV_PART_MAIN);
            lv_obj_remove_flag(sim_row, LV_OBJ_FLAG_SCROLLABLE);

            auto make_sim_btn = [&](const char* text, lv_event_cb_t cb,
                                    lv_obj_t** out_btn, lv_obj_t** out_lbl) {
                lv_obj_t* btn = lv_button_create(sim_row);
                lv_obj_set_height(btn, 34);
                lv_obj_set_flex_grow(btn, 1);
                lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
                lv_obj_set_style_bg_color(btn, lv_color_hex(kColorBtn),
                                          LV_PART_MAIN);
                lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
                lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
                screen_swipe_back_ignore(btn, true);
                lv_obj_t* lbl = lv_label_create(btn);
                lv_label_set_text(lbl, text);
                lv_obj_set_style_text_color(lbl, lv_color_hex(kColorText),
                                            LV_PART_MAIN);
                lv_obj_set_style_text_font(lbl, &font_puhui_20_4, LV_PART_MAIN);
                lv_obj_center(lbl);
                *out_btn = btn;
                *out_lbl = lbl;
            };
            make_sim_btn(I18n::T("外置卡"), on_sim_external_clicked,
                         &s_ui.sim_external_btn, &s_ui.sim_external_lbl);
            make_sim_btn(I18n::T("内置卡"), on_sim_internal_clicked,
                         &s_ui.sim_internal_btn, &s_ui.sim_internal_lbl);

            lv_obj_t* sim_cur = lv_label_create(sim_card);
            s_ui.sim_current_lbl = sim_cur;
            char sim_cur_buf[64];
            snprintf(sim_cur_buf, sizeof(sim_cur_buf), I18n::T("当前：%s"),
                     SimSlotName(GetSavedSimSlot()));
            lv_label_set_text(sim_cur, sim_cur_buf);
            lv_obj_set_style_text_color(sim_cur, lv_color_hex(kColorSubtle),
                                        LV_PART_MAIN);
            lv_obj_set_style_text_font(sim_cur, &font_puhui_20_4, LV_PART_MAIN);
            refresh_sim_slot_ui();
        }
    }

    refresh_network_switch_ui();
}

}  // namespace

// ===========================================================================
// 公共接口
// ===========================================================================
lv_obj_t* NetworkScreen::Create() {
    s_screen_active = true;
    s_scan_in_progress = false;
    s_connect_in_progress = false;
    s_scan_results.clear();

    lv_obj_t* scr = lv_obj_create(nullptr);
    s_ui.screen = scr;
    screen_strip_obj_chrome(scr);
    lv_obj_set_size(scr, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColorBg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_header(scr);
    build_tabview(scr);

    // WiFi 列表在扫描 sheet 内创建；此处只刷新已保存计数。
    rebuild_nearby_list_now();
    rebuild_saved_list_now();

    screen_attach_swipe_back(scr, on_swipe_back);
    lv_obj_add_event_cb(scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED,
                        nullptr);
    return scr;
}

void NetworkScreen::LifecycleCallback(screen_lifecycle_event_t event) {
    if (event == SCREEN_LIFECYCLE_LOAD) {
        ESP_LOGI(TAG, "load: network_screen");
        // 4G 模式下「附近 WiFi」「已保存 WiFi」两个 Tab 都被隐藏，没必要
        // 启动本地 STA 栈做扫描——会无谓地占用 wifi 硬件、抢蜂窝模块的
        // 资源、还可能在 SwitchNetworkType 重启前后产生事件回调毛刺。
        if (!IsCellularMode()) {
            // 不再自动扫描——等用户主动点「扫描附近网络」。
            post_status("", kColorSubtle);
            // 刷新已保存计数（可能用户在外面改过）
            refresh_saved_list();
        }
        refresh_network_switch_ui();
        refresh_sim_slot_ui();
        // 4G 模式下向模组发 AT+ECSIMCFG? 同步真实当前槽位，避免本地 NVS
        // 与模组里实际生效的卡不一致（例如设备外部曾经手动切过卡）。
        if (IsCellularMode()) {
            schedule_sim_slot_query();
        }
    } else {
        ESP_LOGI(TAG, "unload: network_screen");
        s_screen_active = false;
        // 释放我们的 wifi 资源并恢复 WifiStation 接管。函数内部以
        // s_wifi_initialized 短路；4G 模式下未初始化时是 no-op。
        wifi_teardown_for_screen();
    }
}


bool NetworkScreen::IsNavigationBlocked() {
    return screen_alive() && (s_ui.status_overlay != nullptr ||
        s_scan_in_progress || s_connect_in_progress ||
        s_network_switch_pending || s_sim_switch_pending);
}

bool NetworkScreen::TryHandleNavBack() {
    if (!screen_alive()) return false;
    on_swipe_back();
    return true;
}
