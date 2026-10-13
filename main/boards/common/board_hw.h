#pragma once

#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_touch.h>

#ifdef __cplusplus
extern "C" {
#endif

i2c_master_bus_handle_t board_get_i2c_bus(void);
esp_lcd_panel_io_handle_t board_get_panel_io(void);
esp_lcd_panel_handle_t board_get_panel(void);
esp_lcd_touch_handle_t board_get_touch(void);

// 软件关机。返回 true 表示已进入断电/深睡流程；false 表示当前无法软件关机（如 USB 供电）。
bool board_request_shutdown(void);

#ifdef __cplusplus
}
#endif
