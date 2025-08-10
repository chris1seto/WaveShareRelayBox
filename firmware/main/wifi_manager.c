#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "WIFI_MANAGER";

static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;
static bool wifi_connected = false;
static char current_ip[16] = {0};
static int sta_retry_count = 0;

// NVS keys for WiFi credentials
#define NVS_NAMESPACE "wifi_config"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"

static esp_err_t wifi_manager_save_credentials(const char* ssid, const char* password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    err = nvs_set_str(nvs_handle, NVS_KEY_SSID, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save SSID: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_str(nvs_handle, NVS_KEY_PASSWORD, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save password: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }
    
    nvs_close(nvs_handle);
    return err;
}

static esp_err_t wifi_manager_load_credentials(char* ssid, char* password, size_t max_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    size_t ssid_len = max_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, ssid, &ssid_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load SSID: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    size_t password_len = max_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, password, &password_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load password: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    nvs_close(nvs_handle);
    return ESP_OK;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "WiFi AP started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station %s joined, AID=%d", "" /* MAC redacted */, event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station %s left, AID=%d", "" /* MAC redacted */, event->aid);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
        snprintf(current_ip, sizeof(current_ip), IPSTR, IP2STR(&event->ip_info.ip));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected");
        wifi_connected = false;
        memset(current_ip, 0, sizeof(current_ip));
        if (sta_retry_count < WIFI_STA_MAX_RETRIES) {
            sta_retry_count++;
            ESP_LOGI(TAG, "Retrying STA connect (%d/%d)", sta_retry_count, WIFI_STA_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "STA retries exceeded; staying disconnected");
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing WiFi manager");
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create default netif instances
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                      ESP_EVENT_ANY_ID,
                                                      &wifi_event_handler,
                                                      NULL,
                                                      NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                      IP_EVENT_STA_GOT_IP,
                                                      &wifi_event_handler,
                                                      NULL,
                                                      NULL));
    
    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_manager_start_sta(const char* ssid, const char* password)
{
    if (ssid == NULL || password == NULL) {
        ESP_LOGE(TAG, "Invalid SSID or password");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Starting WiFi STA mode");
    
    // Save credentials to NVS
    esp_err_t err = wifi_manager_save_credentials(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save WiFi credentials");
    }
    
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid)-1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password)-1);
    
    // Stop WiFi if already running to safely switch modes
    esp_err_t stop_err = esp_wifi_stop();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_INIT && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop returned: %s", esp_err_to_name(stop_err));
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Explicitly trigger connection attempt
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "WiFi STA mode started; connecting to SSID '%s'", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_start_ap(void)
{
    ESP_LOGI(TAG, "Starting WiFi AP mode");
    
    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(WIFI_AP_SSID),
            .channel = WIFI_AP_CHANNEL,
            .password = WIFI_AP_PASSWORD,
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {
                .required = false
            },
        },
    };
    
    strcpy((char*)wifi_config.ap.ssid, WIFI_AP_SSID);
    strcpy((char*)wifi_config.ap.password, WIFI_AP_PASSWORD);
    
    if (strlen(WIFI_AP_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi AP mode started, SSID: %s, password: %s", WIFI_AP_SSID, WIFI_AP_PASSWORD);
    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    ESP_LOGI(TAG, "Stopping WiFi");
    ESP_ERROR_CHECK(esp_wifi_stop());
    wifi_connected = false;
    memset(current_ip, 0, sizeof(current_ip));
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return wifi_connected;
}

esp_err_t wifi_manager_get_ip(char* ip_str, size_t len)
{
    if (ip_str == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!wifi_connected) {
        return ESP_FAIL;
    }
    
    strncpy(ip_str, current_ip, len - 1);
    ip_str[len - 1] = '\0';
    return ESP_OK;
}

esp_err_t wifi_manager_try_connect_saved(void)
{
    char ssid[33] = {0};
    char password[65] = {0};
    
    esp_err_t err = wifi_manager_load_credentials(ssid, password, sizeof(ssid));
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved WiFi credentials found");
        return err;
    }
    
    ESP_LOGI(TAG, "Found saved WiFi credentials, attempting to connect");
    return wifi_manager_start_sta(ssid, password);
}

esp_err_t wifi_manager_bootstrap(void)
{
    // 1) Try saved credentials
    if (wifi_manager_try_connect_saved() == ESP_OK) {
        ESP_LOGI(TAG, "Attempting connect with saved credentials");
        // Wait for connection or timeout
        int waited = 0;
        while (!wifi_manager_is_connected() && waited < WIFI_STA_CONNECT_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(250));
            waited += 250;
        }
        if (wifi_manager_is_connected()) {
            ESP_LOGI(TAG, "Connected using saved credentials");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Saved credentials failed to connect within timeout");
    }

    // 2) Try fallback credentials if defined
    if (strlen(WIFI_FALLBACK_SSID) > 0) {
        ESP_LOGI(TAG, "Attempting connect with fallback credentials (SSID='%s')", WIFI_FALLBACK_SSID);
        if (wifi_manager_start_sta(WIFI_FALLBACK_SSID, WIFI_FALLBACK_PASSWORD) == ESP_OK) {
            int waited = 0;
            while (!wifi_manager_is_connected() && waited < WIFI_STA_CONNECT_TIMEOUT_MS) {
                vTaskDelay(pdMS_TO_TICKS(250));
                waited += 250;
            }
            if (wifi_manager_is_connected()) {
                ESP_LOGI(TAG, "Connected using fallback credentials");
                return ESP_OK;
            }
            ESP_LOGW(TAG, "Fallback credentials failed to connect within timeout");
        }
    }

    // 3) Start AP mode
    ESP_LOGI(TAG, "Starting AP mode as fallback");
    return wifi_manager_start_ap();
}
