#include "cloudzao_endpoints.h"
#include <string.h>

// Open-source configuration: configure private services before use.
const char cloudzao_host[] = "";
const char xiaozhi_ota_url[] = "https://api.tenclass.net/xiaozhi/ota/";
const char cloudzao_legacy_ota_host[] = "";

const char cloudzao_path_api_v1_prefix[] = "";
const char cloudzao_path_device_prefix[] = "";
const char cloudzao_path_asr_transcribe[] = "";
const char cloudzao_path_asr_audio_records[] = "";
const char cloudzao_path_asr_wav[] = "";
const char cloudzao_path_text2image[] = "";
const char cloudzao_path_text2image_task_fmt[] =
    "";
const char cloudzao_path_sinicloud_token[] = "";
const char cloudzao_path_weather_district[] = "";
const char cloudzao_path_gps_location_report[] = "";
const char cloudzao_path_gps_static_map[] = "";
const char xiaozhi_path_activate[] = "/activate";

int cloudzao_is_legacy_ota_url(const char* url) {
    return url != NULL && cloudzao_legacy_ota_host[0] != '\0' &&
           strstr(url, cloudzao_legacy_ota_host) != NULL;
}
