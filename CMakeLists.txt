cmake_minimum_required(VERSION 3.10)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/generate_build_info.py"
        --metadata "${CMAKE_CURRENT_LIST_DIR}/metadata.json" --print-build-version
    OUTPUT_VARIABLE PROJECT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE release_info_result
)
if(NOT release_info_result EQUAL 0)
    message(FATAL_ERROR "Unable to read the TanRadio release version from metadata.json")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/metadata.json" "${CMAKE_CURRENT_LIST_DIR}/tools/generate_build_info.py")
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/local-components/esp_hal_lcd"
    "${CMAKE_CURRENT_LIST_DIR}/local-components/nokia5110")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(application)

# pax-codecs bundles upstream zlib sources whose legacy definitions and
# deliberate switch fallthroughs are harmless under the project's -Werror.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=old-style-definition" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=implicit-fallthrough" APPEND)
