#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "i18n.h"
#include "system_info.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // Upstream network diagnostics can include URLs, headers and credentials.
    // Keep application status/error logs, but suppress these SDK log sources.
    static const char* const private_network_tags[] = {"HTTP_CLIENT", "esp-tls", "esp-tls-mbedtls",
                            "transport", "transport_base", "transport_ws",
                            "transport_proxy", "mqtt_client", "MQTT_CLIENT"};
    for (const char* tag : private_network_tags) {
        esp_log_level_set(tag, ESP_LOG_NONE);
    }

    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // UI locale from NVS (must run before first screen create).
    I18n::Init();

    // Launch the application
    auto& app = Application::GetInstance();
    app.Start();
}
