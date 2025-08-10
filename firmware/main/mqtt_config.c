#include "mqtt_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "MQTT_CONFIG";

#define MQTT_CONFIG_NAMESPACE "mqtt_config"
#define MQTT_CONFIG_KEY "config"

esp_err_t mqtt_config_init(void)
{
    // Initialize NVS if not already done
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    return ESP_OK;
}

esp_err_t mqtt_config_load(mqtt_config_t* config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(MQTT_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return mqtt_config_get_default(config);
    }
    
    size_t required_size = sizeof(mqtt_config_t);
    ret = nvs_get_blob(nvs_handle, MQTT_CONFIG_KEY, config, &required_size);
    nvs_close(nvs_handle);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load MQTT config from NVS: %s", esp_err_to_name(ret));
        return mqtt_config_get_default(config);
    }
    
    ESP_LOGI(TAG, "MQTT config loaded from NVS");
    return ESP_OK;
}

esp_err_t mqtt_config_save(const mqtt_config_t* config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(MQTT_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = nvs_set_blob(nvs_handle, MQTT_CONFIG_KEY, config, sizeof(mqtt_config_t));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }
    
    nvs_close(nvs_handle);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MQTT config saved to NVS");
    } else {
        ESP_LOGE(TAG, "Failed to save MQTT config to NVS: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t mqtt_config_get_default(mqtt_config_t* config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Set default values
    config->enabled = false;
    strcpy(config->broker, "");
    config->port = 1883;
    strcpy(config->username, "");
    strcpy(config->password, "");
    
    ESP_LOGI(TAG, "Using default MQTT config");
    return ESP_OK;
}

esp_err_t mqtt_config_apply(const mqtt_config_t* config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "MQTT config: enabled=%d, broker=%s, port=%d", 
              config->enabled, config->broker, config->port);
    
    // TODO: Implement MQTT client reconfiguration
    // This would require stopping the current client and reinitializing with new settings
    // For now, just log the configuration and note that a reboot is needed for changes
    
    if (config->enabled) {
        ESP_LOGI(TAG, "MQTT enabled - new settings will take effect after reboot");
    } else {
        ESP_LOGI(TAG, "MQTT disabled - client will stop publishing after reboot");
    }
    
    return ESP_OK;
}
