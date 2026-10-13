#pragma once

#include "cloudzao_endpoints.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace api {

// Logging helpers deliberately accept no payload output, including debug builds.
inline void LogHttpRequest(const char*, const char*, const std::string&,
                           const std::string& = std::string(),
                           const char* = nullptr) {}
inline void LogHttpBinaryRequest(const char*, const char*, const std::string&,
                                 size_t, const char* = nullptr) {}
inline void LogHttpResponse(const char*, int, const std::string&) {}

// Compatibility aliases; all addresses and paths live in cloudzao_endpoints.c.
constexpr const char* kHost = cloudzao_host;

constexpr const char* kApiV1Prefix = cloudzao_path_api_v1_prefix;
constexpr const char* kXiaozhiDevicePrefix = cloudzao_path_device_prefix;

// ASR
constexpr const char* kAsrTranscribe = cloudzao_path_asr_transcribe;
constexpr const char* kAsrAudioRecords =
    cloudzao_path_asr_audio_records;
// 设备侧原始 WAV 转写（octet-stream body）
constexpr const char* kXiaozhiAsrWav = cloudzao_path_asr_wav;

// 百度原子能力文生图
constexpr const char* kText2Image = cloudzao_path_text2image;
// 240 屏画廊约 200px 边；maxSide 略大便于等比放大铺满
constexpr const char* kText2ImageTaskFmt =
    cloudzao_path_text2image_task_fmt;

// Sinicloud 实时同声传译：换 Token，返回 data.wsUrl
constexpr const char* kSinicloudToken = cloudzao_path_sinicloud_token;

// Weather
constexpr const char* kWeatherDistrictPath =
    cloudzao_path_weather_district;

// GPS
constexpr const char* kGpsLocationReport =
    cloudzao_path_gps_location_report;
constexpr const char* kGpsStaticMap =
    cloudzao_path_gps_static_map;

inline std::string Url(const char* path) {
    if (kHost[0] == '\0' || path == nullptr || path[0] == '\0') {
        return {};
    }
    return std::string(kHost) + path;
}

inline std::string WeatherDistrictUrl(const std::string& district_id) {
    const auto url = Url(kWeatherDistrictPath);
    return url.empty() ? url : url + district_id;
}

inline std::string AsrAudioRecordsUrl(const char* original_name) {
    if (original_name == nullptr || original_name[0] == '\0') {
        return Url(kAsrAudioRecords);
    }
    const auto url = Url(kAsrAudioRecords);
    return url.empty() ? url : url + original_name;
}

inline std::string Text2ImageTaskUrl(const std::string& task_id) {
    if (kHost[0] == '\0' || kText2ImageTaskFmt[0] == '\0') {
        return {};
    }
    char path[192];
    std::snprintf(path, sizeof(path), kText2ImageTaskFmt, task_id.c_str());
    return Url(path);
}

// 日志脱敏：响应体等可能含 staticMapUrl 等完整地址，禁止输出 claw 域名 URL。
inline std::string RedactClawUrlsForLog(const std::string& text) {
    std::string out = text;
    const size_t prefix_len = std::strlen(kHost);
    if (prefix_len == 0) {
        return out;
    }
    size_t pos = 0;
    while ((pos = out.find(kHost, pos)) != std::string::npos) {
        size_t end = out.find_first_of("\"' \t\r\n,}", pos + prefix_len);
        if (end == std::string::npos) {
            end = out.size();
        }
        out.replace(pos, end - pos, "[redacted]");
        pos += 10;
    }
    return out;
}

}  // namespace api
