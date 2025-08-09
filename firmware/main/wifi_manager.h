#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_wifi.h"

// WiFi Configuration
#define WIFI_AP_SSID "Waveshare-Relay-AP"
#define WIFI_AP_PASSWORD "12345678"
#define WIFI_AP_CHANNEL 1
#define WIFI_AP_MAX_CONN 4
#define WIFI_AP_BEACON_INTERVAL 100

// Function declarations
esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start_sta(const char* ssid, const char* password);
esp_err_t wifi_manager_start_ap(void);
esp_err_t wifi_manager_stop(void);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_get_ip(char* ip_str, size_t len);
esp_err_t wifi_manager_try_connect_saved(void);

#endif // WIFI_MANAGER_H
