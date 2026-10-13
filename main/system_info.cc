#include "system_info.h"

#include <cstdio>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_wifi_remote.h"
#endif

#include "dual_network_board.h"

#define TAG "SystemInfo"

size_t SystemInfo::GetFlashSize() {
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get flash size");
        return 0;
    }
    return (size_t)flash_size;
}

size_t SystemInfo::GetMinimumFreeHeapSize() {
    return esp_get_minimum_free_heap_size();
}

size_t SystemInfo::GetFreeHeapSize() {
    return esp_get_free_heap_size();
}

std::string SystemInfo::GetMacAddress() {
    uint8_t mac[6];
#if CONFIG_IDF_TARGET_ESP32P4
    const auto* dual = dynamic_cast<const DualNetworkBoard*>(&Board::GetInstance());
    if (dual != nullptr) {
        /* 双网板：统一读主机基址 MAC，不依赖当前选 WiFi 还是 4G、也不触发 esp-hosted RPC */
        esp_err_t err = esp_read_mac(mac, ESP_MAC_BASE);
        (void)err;
    } else {
        esp_wifi_get_mac(WIFI_IF_STA, mac);
    }
#else
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(mac_str);
}

std::string SystemInfo::GetChipModelName() {
    return std::string(CONFIG_IDF_TARGET);
}

std::string SystemInfo::GetUserAgent() {
    auto app_desc = esp_app_get_description();
    auto user_agent = std::string(BOARD_NAME "/") + app_desc->version;
    return user_agent;
}

esp_err_t SystemInfo::PrintTaskCpuUsage(TickType_t xTicksToWait) {
    #define ARRAY_SIZE_OFFSET 5
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    configRUN_TIME_COUNTER_TYPE start_run_time, end_run_time;
    esp_err_t ret;
    uint32_t total_elapsed_time;

    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = (TaskStatus_t*)heap_caps_malloc(sizeof(TaskStatus_t) * start_array_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    vTaskDelay(xTicksToWait);

    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = (TaskStatus_t*)heap_caps_malloc(sizeof(TaskStatus_t) * end_array_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    printf("| Task | Run Time | Percentage\n");
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * CONFIG_FREERTOS_NUMBER_OF_CORES);
            printf("| %-16s | %8lu | %4lu%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;

exit:
    free(start_array);
    free(end_array);
    return ret;
}

void SystemInfo::PrintTaskList() {
    char buffer[1000];
    vTaskList(buffer);
    ESP_LOGI(TAG, "Task list: \n%s", buffer);
}

void SystemInfo::PrintHeapStats() {
    constexpr int kCoreCount = portNUM_PROCESSORS;
    static configRUN_TIME_COUNTER_TYPE s_prev_idle[kCoreCount] = {0};
    static uint64_t s_prev_us = 0;
    static bool s_inited = false;

    const uint64_t now_us = static_cast<uint64_t>(esp_timer_get_time());
    int usage[kCoreCount] = {0};
    int total_usage = 0;

    if (!s_inited) {
        for (int c = 0; c < kCoreCount; ++c) {
            s_prev_idle[c] = ulTaskGetIdleRunTimeCounterForCore(c);
        }
        s_prev_us = now_us;
        s_inited = true;
    } else {
        const uint64_t dt_us = now_us - s_prev_us;
        if (dt_us > 0) {
            for (int c = 0; c < kCoreCount; ++c) {
                const configRUN_TIME_COUNTER_TYPE now_idle =
                    ulTaskGetIdleRunTimeCounterForCore(c);
                const configRUN_TIME_COUNTER_TYPE didle = now_idle - s_prev_idle[c];
                uint64_t idle_pct = static_cast<uint64_t>(didle) * 100ULL / dt_us;
                if (idle_pct > 100) {
                    idle_pct = 100;
                }
                usage[c] = 100 - static_cast<int>(idle_pct);
                total_usage += usage[c];
                s_prev_idle[c] = now_idle;
            }
        }
        s_prev_us = now_us;
    }

    const int avg_cpu = (kCoreCount > 0) ? (total_usage / kCoreCount) : 0;
    const int cpu1 = (kCoreCount > 1) ? usage[1] : 0;

    const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t total_int = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const size_t min_int = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const size_t largest_int =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    const size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t total_ps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t min_ps = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    const size_t free_all = esp_get_free_heap_size();
    const size_t min_all = esp_get_minimum_free_heap_size();

    auto pct = [](size_t free_b, size_t total_b) -> int {
        if (total_b == 0) {
            return 0;
        }
        return static_cast<int>((free_b * 100ULL) / total_b);
    };

    ESP_LOGI(TAG,
             "CPU c0=%d%% c1=%d%% avg=%d%% | "
             "DRAM free=%uKB/%uKB (%d%%) min=%uKB largest=%uKB | "
             "PSRAM free=%uKB/%uKB (%d%%) min=%uKB | "
             "heap free=%uKB min=%uKB",
             usage[0], cpu1, avg_cpu,
             (unsigned)(free_int / 1024), (unsigned)(total_int / 1024),
             pct(free_int, total_int), (unsigned)(min_int / 1024),
             (unsigned)(largest_int / 1024),
             (unsigned)(free_ps / 1024), (unsigned)(total_ps / 1024),
             pct(free_ps, total_ps), (unsigned)(min_ps / 1024),
             (unsigned)(free_all / 1024), (unsigned)(min_all / 1024));
}
