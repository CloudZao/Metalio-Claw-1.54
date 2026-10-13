#include "api_endpoints.h"
#include <cassert>

int main() {
    // Debug callers must not emit addresses, query tokens, or response payloads.
    api::LogHttpRequest("test", "POST", "https://private.example/api?token=secret",
                        "private response", "Authorization: secret");
    api::LogHttpBinaryRequest("test", "POST", "https://private.example/upload", 128,
                              "Authorization: secret");
    api::LogHttpResponse("test", 400, "https://private.example/api?token=secret");
    const bool disabled = api::kHost[0] == '\0' || api::kWeatherDistrictPath[0] == '\0';
    assert(api::Url(nullptr).empty());
    assert(api::Url("").empty());
    assert(api::WeatherDistrictUrl("101").empty() == disabled);
    assert(api::AsrAudioRecordsUrl("sample.wav").empty() == disabled);
    assert(api::AsrAudioRecordsUrl(nullptr).empty() == disabled);
    assert(api::Text2ImageTaskUrl("task").empty() == disabled);
    assert(api::Url(api::kXiaozhiAsrWav).empty() == disabled);
    assert(api::Url(api::kText2Image).empty() == disabled);
    assert(api::Url(api::kSinicloudToken).empty() == disabled);
    assert(api::Url(api::kGpsLocationReport).empty() == disabled);
    assert(api::Url(api::kGpsStaticMap).empty() == disabled);
    const std::string body = std::string("url=") + api::kHost + "/test end";
    assert(api::RedactClawUrlsForLog(body) ==
           (api::kHost[0] == '\0' ? body : "url=[redacted] end"));
    assert(api::RedactClawUrlsForLog("").empty());
    assert(api::RedactClawUrlsForLog("HTTP error") == "HTTP error");
    assert(std::string(xiaozhi_ota_url) == "https://api.tenclass.net/xiaozhi/ota/");
    assert(std::string(xiaozhi_path_activate) == "/activate");
    assert(!cloudzao_is_legacy_ota_url(xiaozhi_ota_url));
    assert(!cloudzao_is_legacy_ota_url("https://custom.example/ota/"));
    assert(!cloudzao_is_legacy_ota_url(nullptr));
    assert(bool(cloudzao_is_legacy_ota_url(cloudzao_legacy_ota_host)) ==
           (cloudzao_legacy_ota_host[0] != '\0'));
    if (!disabled) {
        assert(api::WeatherDistrictUrl("101") == std::string(api::kHost) + api::kWeatherDistrictPath + "101");
        assert(api::Text2ImageTaskUrl("task").find("task") != std::string::npos);
    }
}
