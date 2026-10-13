#include "system_time.h"

#include <atomic>
#include <ctime>
#include <cstring>

#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

namespace {

constexpr const char* TAG = "SystemTime";
std::atomic<bool> s_started{false};
std::atomic<bool> s_starting{false};

void EnsureChinaTz() {
    setenv("TZ", "CST-8", 1);
    tzset();
}

void OnTimeSynced(struct timeval* /*tv*/) {
    time_t now = time(nullptr);
    struct tm tm_info = {};
    localtime_r(&now, &tm_info);
    ESP_LOGI(TAG, "SNTP synced: %04d-%02d-%02d %02d:%02d:%02d",
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
}

// 解析 +CCLK: "yy/MM/dd,hh:mm:ss±zz"；zz 为 15 分钟单位（+32 = UTC+8）。
bool ParseCcLkPayload(const char* p, struct tm* out_tm, int* out_tz_quarter) {
    if (p == nullptr || out_tm == nullptr) {
        return false;
    }
    while (*p == ' ' || *p == '\t' || *p == '"') {
        ++p;
    }

    int yy = 0, mon = 0, day = 0, hh = 0, mm = 0, ss = 0;
    char sign = '+';
    int tzq = 32;
    const int n = std::sscanf(p, "%d/%d/%d,%d:%d:%d%c%d", &yy, &mon, &day, &hh,
                              &mm, &ss, &sign, &tzq);
    if (n < 6) {
        return false;
    }

    std::memset(out_tm, 0, sizeof(*out_tm));
    out_tm->tm_year = (yy >= 100) ? (yy - 1900) : (yy + 100);
    out_tm->tm_mon = mon - 1;
    out_tm->tm_mday = day;
    out_tm->tm_hour = hh;
    out_tm->tm_min = mm;
    out_tm->tm_sec = ss;
    out_tm->tm_isdst = 0;

    if (out_tz_quarter != nullptr) {
        if (n >= 8 && (sign == '+' || sign == '-')) {
            *out_tz_quarter = (sign == '-') ? -tzq : tzq;
        } else {
            *out_tz_quarter = 32;
        }
    }
    return true;
}

// 把时间字段当成 UTC 解释（不依赖 TZ 是否生效）。
time_t MkTimeAsUtc(struct tm tm_info) {
    setenv("TZ", "GMT0", 1);
    tzset();
    return mktime(&tm_info);
}

}  // namespace

void SystemTimeStartSntp() {
    if (s_started.load(std::memory_order_acquire)) {
        return;
    }
    // Only one caller runs init; concurrent callers bail (first wins).
    bool expected = false;
    if (!s_starting.compare_exchange_strong(expected, true,
                                            std::memory_order_acq_rel)) {
        return;
    }
    if (s_started.load(std::memory_order_acquire)) {
        s_starting.store(false, std::memory_order_release);
        return;
    }

    EnsureChinaTz();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    config.sync_cb = OnTimeSynced;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        // Another path already initialized SNTP (e.g. concurrent WiFi hooks).
        s_started.store(true, std::memory_order_release);
        s_starting.store(false, std::memory_order_release);
        ESP_LOGD(TAG, "SNTP already initialized");
        return;
    }
    if (err != ESP_OK) {
        s_starting.store(false, std::memory_order_release);
        ESP_LOGW(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return;
    }
    s_started.store(true, std::memory_order_release);
    s_starting.store(false, std::memory_order_release);
    ESP_LOGI(TAG, "SNTP started (ntp.aliyun.com)");
}

bool SystemTimeApplyModemCcLk(const std::string& response) {
    const size_t pos = response.find("+CCLK:");
    if (pos == std::string::npos) {
        ESP_LOGW(TAG, "no +CCLK in modem response: %s", response.c_str());
        return false;
    }

    struct tm tm_info = {};
    int tz_quarter = 32;
    if (!ParseCcLkPayload(response.c_str() + pos + 6, &tm_info, &tz_quarter)) {
        ESP_LOGW(TAG, "parse +CCLK failed: %s", response.c_str());
        return false;
    }

    // 实测本模组 CCLK 数字是 UTC（例 08:44+32 对应北京 16:44）；±zz 只标时区。
    // 勿再减偏移，否则会再慢 8 小时。
    const time_t epoch = MkTimeAsUtc(tm_info);
    if (epoch < 0) {
        ESP_LOGW(TAG, "mktime failed for +CCLK");
        EnsureChinaTz();
        return false;
    }

    struct timeval tv = {};
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    if (settimeofday(&tv, nullptr) != 0) {
        ESP_LOGW(TAG, "settimeofday failed");
        EnsureChinaTz();
        return false;
    }

    EnsureChinaTz();
    struct tm local_tm = {};
    localtime_r(&epoch, &local_tm);
    ESP_LOGI(TAG, "modem CCLK synced: %04d-%02d-%02d %02d:%02d:%02d (utc fields, tzq=%d)",
             local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
             local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec, tz_quarter);
    return true;
}
