// Waveshare ESP32-S3-Relay-6CH firmware: web UI, REST API, MQTT, mDNS, NTP and OTA.

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "mqtt_link.h"
#include "net_services.h"
#include "ota.h"
#include "relays.h"
#include "settings.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

// A freshly OTA-updated image is confirmed once it has been up this long with the
// web server running and Wi-Fi connected. If it crashes or is reset before that,
// the bootloader rolls back to the previous image.
#define CONFIRM_AFTER_S        30
// ...or after this long even without Wi-Fi, so a router outage can't force a rollback.
#define CONFIRM_FALLBACK_S     300

#define FACTORY_RESET_HOLD_MS  5000

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erasing (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

#if CONFIG_APP_FACTORY_RESET_GPIO >= 0
static void button_task(void *arg)
{
    const gpio_num_t pin = CONFIG_APP_FACTORY_RESET_GPIO;
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);

    int held_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(pin) == 0) {
            held_ms += 100;
            if (held_ms == 1000) {
                ESP_LOGW(TAG, "Keep holding the button for a factory reset...");
            }
            if (held_ms >= FACTORY_RESET_HOLD_MS) {
                settings_factory_reset();
                esp_restart();
            }
        } else {
            held_ms = 0;
        }
    }
}
#endif

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "Waveshare Relay %s (ESP-IDF %s)", app->version, app->idf_ver);

    init_nvs();
    ESP_ERROR_CHECK(settings_init());

    device_settings_t cfg;
    settings_get(&cfg);

    // Relays first so outputs settle as early as possible after reset.
    ESP_ERROR_CHECK(relays_init(cfg.relay_boot));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(wifi_mgr_start(cfg.hostname));
    ESP_ERROR_CHECK(net_services_start(cfg.hostname, cfg.ntp_server, cfg.timezone));
    ESP_ERROR_CHECK(web_server_start());
    ESP_ERROR_CHECK(mqtt_link_start());

#if CONFIG_APP_FACTORY_RESET_GPIO >= 0
    xTaskCreate(button_task, "button", 3072, NULL, 2, NULL);
#endif

    if (ota_pending_verify()) {
        ESP_LOGW(TAG, "Running a new OTA image; it will be confirmed once healthy");
    }
    while (ota_pending_verify()) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        int64_t up_s = esp_timer_get_time() / 1000000;
        wifi_mgr_status_t ws;
        wifi_mgr_get_status(&ws);
        if (web_server_running() &&
            ((ws.sta_connected && up_s >= CONFIRM_AFTER_S) || up_s >= CONFIRM_FALLBACK_S)) {
            ota_mark_valid();
        }
    }
    ESP_LOGI(TAG, "Startup complete, free heap %u bytes", (unsigned)esp_get_free_heap_size());
}
