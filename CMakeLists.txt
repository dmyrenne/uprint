cmake_minimum_required(VERSION 3.16)

# Firmware-Version (in der Weboberfläche sichtbar): im CI aus dem Git-Tag über UPRINT_VERSION,
# lokal ermittelt ESP-IDF sie selbst mit `git describe`
if(DEFINED ENV{UPRINT_VERSION})
    set(PROJECT_VER "$ENV{UPRINT_VERSION}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(uprint)
