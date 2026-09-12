cmake_minimum_required(VERSION 3.16)
# esp_lvgl_port needs an explicit version for a locally overridden LVGL 8.
set(ENV{LVGL_VERSION} "8.3.11")
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4_portable_ui)
