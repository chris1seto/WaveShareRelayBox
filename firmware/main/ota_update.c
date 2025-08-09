#include "ota_update.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_system.h"

static const char *TAG = "OTA_UPDATE";

esp_ota_handle_t ota_handle = 0;
bool ota_in_progress = false;

esp_err_t ota_update_init(void)
{
    ESP_LOGI(TAG, "Initializing OTA update system");
    
    // Check if we have a valid OTA partition
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        ESP_LOGE(TAG, "No running partition found");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Running partition: %s", running->label);
    
    return ESP_OK;
}

esp_err_t ota_update_start(void)
{
    if (ota_in_progress) {
        ESP_LOGE(TAG, "OTA update already in progress");
        return ESP_FAIL;
    }
    
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition available");
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed, error=%d", err);
        return err;
    }
    
    ota_in_progress = true;
    ESP_LOGI(TAG, "OTA update started, partition: %s", update_partition->label);
    
    return ESP_OK;
}

esp_err_t ota_update_write(const uint8_t* data, size_t len)
{
    if (!ota_in_progress) {
        ESP_LOGE(TAG, "OTA update not started");
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_ota_write(ota_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed, error=%d", err);
        return err;
    }
    
    ESP_LOGI(TAG, "Written %d bytes to OTA partition", len);
    return ESP_OK;
}

esp_err_t ota_update_end(void)
{
    if (!ota_in_progress) {
        ESP_LOGE(TAG, "OTA update not started");
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed, error=%d", err);
        ota_in_progress = false;
        return err;
    }
    
    ota_in_progress = false;
    ESP_LOGI(TAG, "OTA update completed successfully");
    
    // Set boot partition to the updated partition
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition != NULL) {
        err = esp_ota_set_boot_partition(update_partition);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed, error=%d", err);
            return err;
        }
        ESP_LOGI(TAG, "Boot partition set to: %s", update_partition->label);
    }
    
    return ESP_OK;
}

esp_err_t ota_update_abort(void)
{
    if (!ota_in_progress) {
        ESP_LOGE(TAG, "OTA update not started");
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_ota_abort(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_abort failed, error=%d", err);
        return err;
    }
    
    ota_in_progress = false;
    ESP_LOGI(TAG, "OTA update aborted");
    
    return ESP_OK;
}
