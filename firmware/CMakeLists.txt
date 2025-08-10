cmake_minimum_required(VERSION 3.16)

# Set the project name
set(PROJECT_NAME "waveshare-relay-firmware")

# Include ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Define the project
project(${PROJECT_NAME})

# Set target to ESP32-S3
set(IDF_TARGET "esp32s3")

# Package LittleFS partition image from the 'data' directory in this project
# Requires the 'esp_littlefs' component via the component manager
littlefs_create_partition_image(littlefs data FLASH_IN_PROJECT)
