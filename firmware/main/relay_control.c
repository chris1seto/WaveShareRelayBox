#include "relay_control.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "app_mqtt.h"

static const char *TAG = "RELAY_CONTROL";

// Relay GPIO pins array
const int relay_gpios[NUM_RELAYS] = {
    RELAY_1_GPIO,
    RELAY_2_GPIO,
    RELAY_3_GPIO,
    RELAY_4_GPIO,
    RELAY_5_GPIO,
    RELAY_6_GPIO
};

// Current relay states
int relay_states[NUM_RELAYS] = {0};

esp_err_t relay_control_init(void)
{
    ESP_LOGI(TAG, "Initializing relay control");
    
    // Configure GPIO pins for relays
    for (int i = 0; i < NUM_RELAYS; i++) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << relay_gpios[i]),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        
        esp_err_t ret = gpio_config(&io_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure GPIO %d for relay %d", relay_gpios[i], i);
            return ret;
        }
        
        // Initialize relay to OFF state
        gpio_set_level(relay_gpios[i], RELAY_OFF);
        relay_states[i] = RELAY_OFF;
        
        ESP_LOGI(TAG, "Relay %d initialized on GPIO %d", i, relay_gpios[i]);
    }
    
    ESP_LOGI(TAG, "Relay control initialized successfully");
    return ESP_OK;
}

esp_err_t relay_set_state(int relay_id, bool state)
{
    if (relay_id < 0 || relay_id >= NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Setting relay %d to %s", relay_id, state ? "ON" : "OFF");
    
    gpio_set_level(relay_gpios[relay_id], state ? RELAY_ON : RELAY_OFF);
    relay_states[relay_id] = state ? RELAY_ON : RELAY_OFF;
    
    return ESP_OK;
}

bool relay_get_state(int relay_id)
{
    if (relay_id < 0 || relay_id >= NUM_RELAYS) {
        ESP_LOGE(TAG, "Invalid relay ID: %d", relay_id);
        return false;
    }
    
    return relay_states[relay_id] == RELAY_ON;
}

esp_err_t relay_set_multiple(const char* json_data)
{
    cJSON *json = cJSON_Parse(json_data);
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON data");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ESP_OK;
    cJSON *item = json->child;
    
    while (item != NULL) {
        if (cJSON_IsNumber(item)) {
            int relay_id = item->valueint;
            bool state = cJSON_IsTrue(item);
            
            if (relay_id >= 0 && relay_id < NUM_RELAYS) {
                esp_err_t set_ret = relay_set_state(relay_id, state);
                if (set_ret != ESP_OK) {
                    ret = set_ret;
                }
            } else {
                ESP_LOGE(TAG, "Invalid relay ID in JSON: %d", relay_id);
                ret = ESP_ERR_INVALID_ARG;
            }
        }
        item = item->next;
    }
    
    cJSON_Delete(json);
    
    if (ret == ESP_OK) {
        // Publish status after successful update
        relay_publish_status();
    }
    
    return ret;
}

void relay_publish_status(void)
{
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object for status");
        return;
    }
    
    for (int i = 0; i < NUM_RELAYS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "%d", i);
        cJSON_AddBoolToObject(json, key, relay_states[i] == RELAY_ON);
    }
    
    char *json_string = cJSON_Print(json);
    if (json_string != NULL) {
        // Publish status via MQTT
        mqtt_publish_status(json_string);
        ESP_LOGI(TAG, "Relay status: %s", json_string);
        free(json_string);
    }
    
    cJSON_Delete(json);
}
