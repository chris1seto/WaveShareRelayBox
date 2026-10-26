#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    MQTT_LINK_DISABLED,
    MQTT_LINK_CONNECTING,
    MQTT_LINK_CONNECTED,
} mqtt_link_state_t;

/**
 * Start the MQTT worker. Topics (root = configured root topic, relays numbered 0-5):
 *   <root>/status              retained JSON {"0":true,...}
 *   <root>/set                 JSON command, same format (or "toggle" values)
 *   <root>/relay/<n>/state     retained ON/OFF
 *   <root>/relay/<n>/set       ON / OFF / TOGGLE
 *   <root>/availability        retained online/offline (last will)
 *   <root>/info                retained device info JSON
 * Plus Home Assistant discovery under homeassistant/switch/... when enabled.
 */
esp_err_t mqtt_link_start(void);

/** Re-read settings and reconnect. Safe to call from any task. */
void mqtt_link_reload(void);

mqtt_link_state_t mqtt_link_state(void);
