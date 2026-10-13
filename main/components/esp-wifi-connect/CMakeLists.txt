# Compress provisioning pages without changing their source or UI behavior.
set(PAGES_DIR "${CMAKE_CURRENT_BINARY_DIR}/pages")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/assets/wifi_configuration.html"
    "${CMAKE_CURRENT_LIST_DIR}/assets/wifi_configuration_done.html"
    "${CMAKE_CURRENT_LIST_DIR}/tools/compress_pages.py")
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/tools/compress_pages.py"
    "${CMAKE_CURRENT_LIST_DIR}/assets" "${PAGES_DIR}"
    RESULT_VARIABLE PAGES_RESULT)
if(NOT PAGES_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not compress Wi-Fi provisioning pages")
endif()
idf_component_register(
    SRCS
        "wifi_manager.cc"
        "wifi_configuration_ap.cc"
        "wifi_station.cc"
        "ssid_manager.cc"
        "dns_server.cc"
    INCLUDE_DIRS
        "include"
    EMBED_FILES
        "${PAGES_DIR}/wifi_configuration.html.gz"
        "${PAGES_DIR}/wifi_configuration_done.html.gz"
    REQUIRES
    	"esp_timer"
        "esp_http_server"
        "esp_wifi"
        "nvs_flash"
        "json"
)
