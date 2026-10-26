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
        esp_timer
        mbedtls
    # The web UI is compiled into the firmware so a single OTA image updates everything.
    EMBED_FILES "../web/index.html"
)
