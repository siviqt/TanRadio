idf_build_get_property(idf_target IDF_TARGET)

if("${idf_target}" STREQUAL "esp32p4")
    list(APPEND extra_requires esp-hosted-tanmatsu)
endif()

idf_component_register(
    SRCS
        "main.c"
        "app/app.c"
        "audio/audio.c"
        "audio/pcm.c"
        "input/input.c"
        "input/keys.c"
        "network/network.c"
        "radio/radio.c"
        "radio/icy.c"
        "radio/artwork.c"
        "external_display/external_display.c"
        "external_display/nokia.c"
        "radio/stations.c"
        "radio/browser.c"
        "radio/browser_parse.c"
        "storage/sdcard.c"
        "ui/ui.c"
        "ui/controls.c"
        "ui/exit.c"
        "ui/settings.c"
        "ui/about.c"
        "ui/about_view.c"
        "ui/welcome.c"
        "util/util.c"
    EMBED_FILES "ui/assets/tanradio-logo-display.png"
    PRIV_REQUIRES
        esp_lcd
        esp_driver_jpeg
        nokia5110
        esp_app_format
        fatfs
        nvs_flash
        esp_http_client
        esp_netif
        esp_timer
        lwip
        espressif__cjson
        bt
        badge-bsp
        tanmatsu-wifi
        mbedtls
        ${extra_requires}
    INCLUDE_DIRS
        "."
)

# Release metadata is the single source for the user-facing version/revision.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    idf_build_get_property(build_python PYTHON)
    idf_component_get_property(about_bsp_version badgeteam__badge-bsp COMPONENT_VERSION)
    idf_component_get_property(about_lcd_version nicolaielectronics__mipi_dsi_abstraction COMPONENT_VERSION)
    idf_component_get_property(about_codec_version nicolaielectronics__es8156 COMPONENT_VERSION)
    execute_process(
        COMMAND "${build_python}" "${PROJECT_DIR}/tools/generate_build_info.py"
            --metadata "${PROJECT_DIR}/metadata.json"
            --output "${CMAKE_CURRENT_BINARY_DIR}/generated/tanradio_build_info.h"
            --bsp "${about_bsp_version}" --lcd "${about_lcd_version}" --codec "${about_codec_version}"
        RESULT_VARIABLE about_info_result
    )
    if(NOT about_info_result EQUAL 0)
        message(FATAL_ERROR "Unable to generate TanRadio version information from metadata.json")
endif()
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_DIR}/metadata.json" "${PROJECT_DIR}/tools/generate_build_info.py")
endif()

idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)

# Keep compiler stack accounting for the search-render regression check.
set_source_files_properties("ui/ui.c" "ui/about_view.c" "ui/settings.c" "radio/stations.c"
    PROPERTIES COMPILE_OPTIONS "-fstack-usage")
