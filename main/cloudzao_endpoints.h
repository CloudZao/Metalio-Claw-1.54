#pragma once

// Central URL configuration, shared by C and C++ callers.
#ifdef __cplusplus
extern "C" {
#endif

extern const char cloudzao_host[];
extern const char xiaozhi_ota_url[];
// Historical invalid OTA host stored in NVS; keep independent of the API host.
extern const char cloudzao_legacy_ota_host[];
extern const char cloudzao_path_api_v1_prefix[];
extern const char cloudzao_path_device_prefix[];
extern const char cloudzao_path_asr_transcribe[];
extern const char cloudzao_path_asr_audio_records[];
extern const char cloudzao_path_asr_wav[];
extern const char cloudzao_path_text2image[];
extern const char cloudzao_path_text2image_task_fmt[];
extern const char cloudzao_path_sinicloud_token[];
extern const char cloudzao_path_weather_district[];
extern const char cloudzao_path_gps_location_report[];
extern const char cloudzao_path_gps_static_map[];
extern const char xiaozhi_path_activate[];
// An empty legacy host disables migration instead of matching every URL.
int cloudzao_is_legacy_ota_url(const char* url);

#ifdef __cplusplus
}
#endif
