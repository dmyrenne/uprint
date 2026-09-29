cmake_minimum_required(VERSION 3.16)

# Board-Variante:
#   n16r8  ESP32-S3 mit 16 MB Flash und 8 MB Octal-PSRAM, microSD und interner Dateispeicher (Standard)
#   basic  jedes ESP32-S3 ab 4 MB Flash, Dateien nur auf microSD
# Auswahl: idf.py -DUPRINT_VARIANT=basic -B build-basic build
set(UPRINT_VARIANT "n16r8" CACHE STRING "Board-Variante: n16r8 oder basic")
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.${UPRINT_VARIANT}")
    message(FATAL_ERROR "Unbekannte Variante '${UPRINT_VARIANT}' (erlaubt: n16r8, basic)")
endif()
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.${UPRINT_VARIANT}")
# Eigene sdkconfig je Build-Verzeichnis, damit sich die Varianten nicht vermischen
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

# Firmware-Version (in der Weboberfläche sichtbar): im CI aus dem Git-Tag über UPRINT_VERSION,
# lokal ermittelt ESP-IDF sie selbst mit `git describe`
if(DEFINED ENV{UPRINT_VERSION})
    set(PROJECT_VER "$ENV{UPRINT_VERSION}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(COMPILE_DEFINITIONS "UPRINT_VARIANT=\"${UPRINT_VARIANT}\"" APPEND)
project(uprint)
