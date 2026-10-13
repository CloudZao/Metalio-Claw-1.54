#include "lv_adapter_display.h"

#include <cstring>
#include <memory>

#include <esp_lcd_panel_io.h>
#include <esp_log.h>

#include "esp_lv_adapter.h"
#include "esp_lv_fs.h"
#include "esp_mmap_assets.h"
#include "touch_feed.h"   

#include "mmap_generate_resources.h"

#include "screen/boot_screen/boot_screen.h"
#include "screen/chat_screen/chat_screen.h"
#include "screen/home_screen/home_screen.h"

#include "application.h"

static const char* TAG = "LVAdapterDisplay";

LVAdapterDisplay::LVAdapterDisplay(const esp_lcd_panel_handle_t panel,
                                   const esp_lcd_panel_io_handle_t panel_io,
                                   const esp_lcd_touch_handle_t touch_handle, const int width,
                                   const int height) {
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    // S3：LVGL 任务里会 mmap 读 resources / 偶发读 NVS。栈若在 PSRAM，
    // Flash 关 cache 时会触发 esp_task_stack_is_sane_cache_disabled()。
#if CONFIG_IDF_TARGET_ESP32S3
    adapter_cfg.stack_in_psram = false;
#else
    adapter_cfg.stack_in_psram = true;
#endif
    adapter_cfg.task_priority = 1;
    adapter_cfg.task_core_id = 1;

    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    const uint16_t hor = static_cast<uint16_t>(width);
    const uint16_t ver = static_cast<uint16_t>(height);
#if CONFIG_IDF_TARGET_ESP32S3
    // 240 屏：内部 RAM 分块刷（约 40 行），避免 PSRAM 全帧双缓冲在 SPI DMA
    // bounce 时反复 ESP_ERR_NO_MEM，导致只有顶栏小字能画、图标区花屏/灰块。
    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(
            panel, panel_io, hor, ver, ESP_LV_ADAPTER_ROTATE_0);
    disp_cfg.profile.buffer_height = 40;
#else
    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(
            panel, panel_io, hor, ver, ESP_LV_ADAPTER_ROTATE_0);
#endif

    lv_display_t* disp = esp_lv_adapter_register_display(&disp_cfg);
    if (touch_handle != nullptr) {
        esp_lv_adapter_touch_config_t touch_cfg =
            ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
        lv_indev_t* touch_indev = esp_lv_adapter_register_touch(&touch_cfg);
        touch_feed_init(touch_handle, 20);
        touch_feed_attach_indev(touch_indev);
    } else {
        ESP_LOGW(TAG, "No touch handle, skip indev registration");
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    mmap_assets_handle_t assets;
    const mmap_assets_config_t mmap_cfg = {
        .partition_label = "resources",
        .max_files = MMAP_RESOURCES_FILES,
        .checksum = MMAP_RESOURCES_CHECKSUM,
        .flags = {.mmap_enable = true},
    };
    ESP_ERROR_CHECK(mmap_assets_new(&mmap_cfg, &assets));

    esp_lv_fs_handle_t fs_handle;
    const fs_cfg_t fs_cfg = {
        .fs_letter = 'A',
        .fs_nums = MMAP_RESOURCES_FILES,
        .fs_assets = assets,
    };
    ESP_ERROR_CHECK(esp_lv_adapter_fs_mount(&fs_cfg, &fs_handle));

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        SetupUI();
        esp_lv_adapter_unlock();
    }

    // Application::GetInstance().ScheduleStopVoiceUiSession();  // legacy ForceReturnToIdle removed
}

void LVAdapterDisplay::SetupUI() {
    lv_obj_t* boot_scr = BootScreen::Create();
    lv_screen_load(boot_scr);

    lv_timer_t* timer = lv_timer_create(
        [](lv_timer_t* t) {
            lv_obj_t* old_scr = lv_screen_active();

            if (esp_lv_adapter_lock(-1) == ESP_OK) {
                lv_obj_t* home_scr = HomeScreen::Create();
                lv_screen_load(home_scr);
                if (old_scr != NULL && old_scr != home_scr) {
                    lv_obj_delete(old_scr);
                }
                esp_lv_adapter_unlock();
            }

            lv_timer_delete(t);
        },
        2000, nullptr);
    lv_timer_set_repeat_count(timer, 1);
}

LVAdapterDisplay::~LVAdapterDisplay() = default;

void LVAdapterDisplay::SetEmotion(const char* const emotion) {
    // 聊天屏表情模式用服务器原始情绪名
    // （对应 S:/sdcard/system/chat/{emotion}.eaf）。这把锁和
    // SetChatMessage 共用，确保 lv_eaf_set_src 与屏幕生命周期互斥。
    // 屏幕不在前台时 SetEmotion 只更新静态缓存，下次 Create / 切到
    // 表情模式再加载，故无需判断 IsActive()。
    ESP_LOGI(TAG, "SetEmotion: %s",
             emotion != nullptr ? emotion : "<null>");

    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    ChatScreen::SetEmotion(emotion != nullptr ? emotion : "neutral");
    esp_lv_adapter_unlock();
}

void LVAdapterDisplay::SetChatMessage(const char* const role, const char* const content) {
    if (role == nullptr || content == nullptr || content[0] == '\0') {
        return;
    }

    // role 归一化：
    //   user             -> 用户发言
    //   assistant/system -> 设备 / AI 回应
    const bool is_user = (std::strcmp(role, "user") == 0);
    const bool is_bot  = (std::strcmp(role, "assistant") == 0 ||
                          std::strcmp(role, "system") == 0);
    if (!is_user && !is_bot) {
        return;
    }

    // 仅聊天屏在前台时展示气泡；其它屏直接丢弃。
    if (!ChatScreen::IsActive()) {
        return;
    }

    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return;
    }
    ChatScreen::AddMessage(content,
                           is_user ? ChatMsgDir::Right : ChatMsgDir::Left);
    esp_lv_adapter_unlock();
}

void LVAdapterDisplay::SetStatus(const char* const status) {}

void LVAdapterDisplay::ShowNotification(const char* notification, int duration_ms) {}

void LVAdapterDisplay::UpdateStatusBar(bool update_all) {}

void LVAdapterDisplay::SetPowerSaveMode(bool on) {}

void LVAdapterDisplay::SetPreviewImage(const void* image) {}

void LVAdapterDisplay::SetTheme(Theme* const theme) { ESP_LOGI(TAG, "SetTheme: %p", theme); }

bool LVAdapterDisplay::Lock(const int timeout_ms) { return true; }

void LVAdapterDisplay::Unlock() {}
