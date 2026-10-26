cmake_minimum_required(VERSION 3.22)

# Firmware version reported in the web UI, MQTT and the OTA image header.
set(PROJECT_VER "2.0.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(waveshare-relay-firmware)
