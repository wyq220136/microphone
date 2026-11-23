cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Use the same installed Arduino source as the existing sketch. Override with
# -DARDUINO_COMPONENT_DIR=... on other machines; never mix prebuilt SDK libraries.
set(ARDUINO_COMPONENT_DIR "C:/Users/wyq/AppData/Local/Arduino15/packages/esp32/hardware/esp32/3.3.12"
    CACHE PATH "Arduino ESP32 3.3.12 source directory")
set(EXTRA_COMPONENT_DIRS "${ARDUINO_COMPONENT_DIR}")
get_filename_component(ARDUINO_COMPONENT_NAME "${ARDUINO_COMPONENT_DIR}" NAME)
set(ENV{SINGLEMIC_ARDUINO_COMPONENT_NAME} "${ARDUINO_COMPONENT_NAME}")
project(singlemic_gtcrn)
# sdkconfig.defaults only affects fresh configurations. Override the effective
# application flags too, so an existing sdkconfig selecting -Og/-O2 cannot
# silently change the SPP sender's optimization after switching branches.
idf_build_get_property(singlemic_compile_options COMPILE_OPTIONS)
list(FILTER singlemic_compile_options EXCLUDE REGEX "^-O")
list(APPEND singlemic_compile_options "-Os")
idf_build_set_property(COMPILE_OPTIONS "${singlemic_compile_options}")
message(STATUS "singlemic: forcing application compiler optimization to -Os")
# Arduino declares Modbus as a public dependency even when no Modbus API is
# linked. GCC 15 diagnoses its upstream bounded strncpy as an error; keep the
# exception confined to that external target, retaining warnings in our code.
idf_component_get_property(modbus_lib espressif__esp-modbus COMPONENT_LIB)
target_compile_options(${modbus_lib} PRIVATE -Wno-error=stringop-truncation)
if(NOT IDF_VERSION_MAJOR EQUAL 6 OR NOT IDF_VERSION_MINOR EQUAL 1)
    message(FATAL_ERROR "This integration is validated against ESP-IDF 6.1")
endif()
