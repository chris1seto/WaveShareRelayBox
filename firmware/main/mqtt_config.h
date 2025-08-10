#ifndef MQTT_CONFIG_H
#define MQTT_CONFIG_H

#include "esp_err.h"
#include <stdbool.h>

// MQTT Configuration structure
typedef struct {
    bool enabled;
    char broker[64];
    int port;
    char username[32];
    char password[64];
} mqtt_config_t;

// Function declarations
esp_err_t mqtt_config_init(void);
esp_err_t mqtt_config_load(mqtt_config_t* config);
esp_err_t mqtt_config_save(const mqtt_config_t* config);
esp_err_t mqtt_config_get_default(mqtt_config_t* config);
esp_err_t mqtt_config_apply(const mqtt_config_t* config);

#endif // MQTT_CONFIG_H
