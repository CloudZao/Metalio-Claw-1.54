#include "application.h"
#include "board_hw.h"
#include "button.h"
#include "config.h"
#include "display/lv_adapter_display.h"
#include "dual_network_board.h"
#include "pwr_key_handler.h"

#include "codecs/es8311_audio_codec.h"
#include "power_manager.h"

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_touch_cst816s.h>
#include <esp_log.h>
#include <esp_lv_adapter.h>

#define TAG "METALIO_CLAW_1_54"

static esp_lcd_panel_io_handle_t s_panel_io = nullptr;
static esp_lcd_panel_handle_t s_panel = nullptr;
static esp_lcd_touch_handle_t s_touch = nullptr;
static i2c_master_bus_handle_t s_i2c_bus = nullptr;
static PowerManager* s_power_manager = nullptr;

extern "C" i2c_master_bus_handle_t board_get_i2c_bus(void) { return s_i2c_bus; }
extern "C" esp_lcd_panel_io_handle_t board_get_panel_io(void) { return s_panel_io; }
extern "C" esp_lcd_panel_handle_t board_get_panel(void) { return s_panel; }
extern "C" esp_lcd_touch_handle_t board_get_touch(void) { return s_touch; }

extern "C" i2c_master_bus_handle_t metalio_claw_4_get_i2c_bus(void) {
    return board_get_i2c_bus();
}
extern "C" esp_lcd_panel_io_handle_t metalio_claw_4_get_panel_io(void) {
    return board_get_panel_io();
}
extern "C" esp_lcd_panel_handle_t metalio_claw_4_get_panel(void) {
    return board_get_panel();
}
extern "C" esp_lcd_touch_handle_t metalio_claw_4_get_touch(void) {
    return board_get_touch();
}

extern "C" bool board_request_shutdown(void) {
    if (s_power_manager != nullptr) {
        return s_power_manager->shutdown();
    }
    return false;
}

class METALIO_CLAW_1_54 : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Display* display_ = nullptr;
    PowerManager* power_manager_ = nullptr;
    bool touch_found_ = false;

    void InitializePowerManager() {
        power_manager_ = new PowerManager(POWER_USB_IN);
        s_power_manager = power_manager_;
    }

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags =
                {
                    .enable_internal_pullup = 1,
                },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
        s_i2c_bus = i2c_bus_;

        for (uint8_t addr = 1; addr < 127; addr++) {
            if (i2c_master_probe(i2c_bus_, addr, 100) == ESP_OK) {
                ESP_LOGI(TAG, "I2C device at 0x%02X", addr);
                if (addr == 0x15) {
                    touch_found_ = true;
                }
            }
        }
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SDA;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCL;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        // SPI 单次最大传输量：40 行 RGB565 像素。
        buscfg.max_transfer_sz = DISPLAY_WIDTH * 40 * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS;
        io_config.dc_gpio_num = DISPLAY_DC;
        io_config.spi_mode = 3;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 2;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &s_panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RES;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    }

    void InitializeTouch() {
        if (!touch_found_) {
            ESP_LOGW(TAG, "CST816 not found at 0x15, touch disabled");
            return;
        }

        esp_lcd_panel_io_handle_t tp_io = nullptr;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {};
        tp_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS;
        tp_io_config.control_phase_bytes = 1;
        tp_io_config.dc_bit_offset = 0;
        tp_io_config.lcd_cmd_bits = 8;
        tp_io_config.lcd_param_bits = 0;
        tp_io_config.flags.disable_control_phase = 1;
        tp_io_config.scl_speed_hz = 400 * 1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io));

        const esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH,
            .y_max = DISPLAY_HEIGHT,
            .rst_gpio_num = GPIO_NUM_NC,
            .int_gpio_num = GPIO_NUM_NC,
            .levels =
                {
                    .reset = 0,
                    .interrupt = 0,
                },
            .flags =
                {
                    .swap_xy = TOUCH_SWAP_XY,
                    .mirror_x = TOUCH_MIRROR_X,
                    .mirror_y = TOUCH_MIRROR_Y,
                },
        };
        esp_err_t err = esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &s_touch);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CST816 init failed: %s", esp_err_to_name(err));
            s_touch = nullptr;
        }
    }

    void InitializeDisplay() {
        display_ = new LVAdapterDisplay(s_panel, s_panel_io, s_touch, DISPLAY_WIDTH,
                                        DISPLAY_HEIGHT);
    }

public:
    METALIO_CLAW_1_54() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, 1) {
        InitializePowerManager();
        InitializeI2c();
        PwrKey_Init(Power_Dec, false);
        // 须在 PwrKey_Init 之后：插电边沿用来抑制 GPIO47 毛刺短按。
        if (power_manager_ != nullptr) {
            power_manager_->OnChargingStatusChanged([](bool charging) {
                PwrKey_OnUsbPowerChanged(charging);
            });
        }
        InitializeSpi();
        InitializeSt7789Display();
        InitializeTouch();
        InitializeDisplay();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE,
                                            AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
                                            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                                            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
                                            AUDIO_CODEC_I2C_PA_EN, AUDIO_CODEC_ES8311_ADDR,
                                            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (power_manager_ == nullptr) {
            return false;
        }
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        level = power_manager_->GetBatteryLevel();
        return true;
    }
};

DECLARE_BOARD(METALIO_CLAW_1_54);
