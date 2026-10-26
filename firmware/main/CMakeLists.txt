idf_component_register(
    SRCS
        "app_main.c"
        "settings.c"
        "relays.c"
        "wifi_mgr.c"
        "net_services.c"
        "mqtt_link.c"
        "ota.c"
        "web_server.c"
        "api.c"
        "scheduler.c"
        "board_io.c"
        "modbus.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        app_update
        esp_app_format
        bootloader_support
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_uart
        esp_timer
        mbedtls
    # The web UI is compiled into the firmware so a single OTA image updates everything.
    EMBED_FILES "../web/index.html"
)

# Regenerate the build timestamp on every build.
set(build_time_c "${CMAKE_CURRENT_BINARY_DIR}/build_time.c")
add_custom_target(gen_build_time
    COMMAND ${CMAKE_COMMAND} -DOUT=${build_time_c} -P ${CMAKE_CURRENT_SOURCE_DIR}/build_time.cmake
    BYPRODUCTS ${build_time_c}
    VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE ${build_time_c})
add_dependencies(${COMPONENT_LIB} gen_build_time)
