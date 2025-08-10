// Renamed from mqtt_client.h to avoid collision with ESP-IDF's <mqtt_client.h>
#ifndef APP_MQTT_H
#define APP_MQTT_H

#include "esp_err.h"
#include <mqtt_client.h>

// MQTT Configuration - now loaded from NVS
#define MQTT_CLIENT_ID "waveshare-relay-esp32s3"

// MQTT Topics
#define MQTT_TOPIC_ROOT "waveshare/relay"
#define MQTT_TOPIC_SET "/set"
#define MQTT_TOPIC_STATUS "/status"
#define MQTT_TOPIC_CONFIG "/config"

// Function declarations
esp_err_t mqtt_client_init(void);
esp_err_t mqtt_client_start(void);
esp_err_t mqtt_client_stop(void);
esp_err_t mqtt_publish_status(const char* status_json);
esp_err_t mqtt_publish_config(const char* config_json);
bool mqtt_is_enabled(void);

// External variables
extern esp_mqtt_client_handle_t mqtt_client;

#endif // APP_MQTT_H
