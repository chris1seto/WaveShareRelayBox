#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include "esp_err.h"
#include "esp_ota_ops.h"

// OTA Configuration
#define OTA_BUFFER_SIZE 1024
#define OTA_RECV_TIMEOUT 5000

// Function declarations
esp_err_t ota_update_init(void);
esp_err_t ota_update_start(void);
esp_err_t ota_update_write(const uint8_t* data, size_t len);
esp_err_t ota_update_end(void);
esp_err_t ota_update_abort(void);

// External variables
extern esp_ota_handle_t ota_handle;
extern bool ota_in_progress;

#endif // OTA_UPDATE_H
