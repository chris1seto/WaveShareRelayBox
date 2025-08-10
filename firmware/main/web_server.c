#include "web_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "relay_control.h"
#include "wifi_manager.h"
#include "app_mqtt.h"
#include "mqtt_config.h"
#include "esp_wifi.h"
#include <string.h>

static const char *TAG = "WEB_SERVER";

static httpd_handle_t server = NULL;

// Serve index.html from LittleFS
#include <stdio.h>

esp_err_t web_server_get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    FILE *f = fopen("/www/index.html", "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "index.html not found");
        return ESP_FAIL;
    }
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t web_server_get_status(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /status");
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON");
        return ESP_FAIL;
    }
    
    // Get relay states
    cJSON *relays = cJSON_CreateObject();
    for (int i = 0; i < NUM_RELAYS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "%d", i);
        cJSON_AddBoolToObject(relays, key, relay_get_state(i));
    }
    cJSON_AddItemToObject(json, "relays", relays);
    
    // Get system status
    cJSON_AddBoolToObject(json, "wifi_connected", wifi_manager_is_connected());
    
    char ip_str[16];
    if (wifi_manager_get_ip(ip_str, sizeof(ip_str)) == ESP_OK) {
        cJSON_AddStringToObject(json, "ip_address", ip_str);
    } else {
        cJSON_AddStringToObject(json, "ip_address", "N/A");
    }
    
    // Get current SSID if connected
    char ssid_str[33];
    if (wifi_manager_get_current_ssid(ssid_str, sizeof(ssid_str)) == ESP_OK) {
        cJSON_AddStringToObject(json, "current_ssid", ssid_str);
    } else {
        cJSON_AddStringToObject(json, "current_ssid", "");
    }
    
    cJSON_AddBoolToObject(json, "mqtt_connected", false); // TODO: implement MQTT status
    
    // Load MQTT configuration from NVS
    mqtt_config_t mqtt_config;
    if (mqtt_config_load(&mqtt_config) == ESP_OK) {
        cJSON_AddBoolToObject(json, "mqtt_enabled", mqtt_config.enabled);
        cJSON_AddStringToObject(json, "mqtt_broker", mqtt_config.broker);
        cJSON_AddNumberToObject(json, "mqtt_port", mqtt_config.port);
    } else {
        // Fallback to defaults if NVS load fails
        cJSON_AddBoolToObject(json, "mqtt_enabled", false);
        cJSON_AddStringToObject(json, "mqtt_broker", "");
        cJSON_AddNumberToObject(json, "mqtt_port", 1883);
    }
    cJSON_AddNumberToObject(json, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(json, "uptime", esp_timer_get_time() / 1000000);
    cJSON_AddStringToObject(json, "firmware_version", "1.0.0");
    
    char *json_string = cJSON_Print(json);
    if (json_string != NULL) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_string, strlen(json_string));
        free(json_string);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to print JSON");
    }
    
    cJSON_Delete(json);
    return ESP_OK;
}

esp_err_t web_server_post_relay(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /relay len=%d", (int)req->content_len);
    
    // Check content length
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    if (req->content_len >= 1024) { // Reduced from 4096 to 1024
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Body too large");
        return ESP_FAIL;
    }

    // Use a smaller buffer to avoid stack overflow
    char content[1024];
    int total_len = req->content_len;
    int cur_len = 0;
    
    // Read data in smaller chunks to be safer
    while (cur_len < total_len) {
        int chunk_size = (total_len - cur_len) > 256 ? 256 : (total_len - cur_len);
        int received = httpd_req_recv(req, content + cur_len, chunk_size);
        
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Failed to receive data: %d", received);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    content[cur_len] = '\0';

    ESP_LOGI(TAG, "Received relay control: %s", content);

    esp_err_t ret = relay_set_multiple(content);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to set relay state");
        return ESP_FAIL;
    }
    
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

esp_err_t web_server_post_wifi(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /wifi len=%d", (int)req->content_len);
    
    // Check content length
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }

    if (req->content_len >= 1024) { // Reduced from 2048 to 1024
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "Body too large");
        return ESP_FAIL;
    }

    // Use a smaller buffer to avoid stack overflow
    char content[1024];
    int total_len = req->content_len;
    int cur_len = 0;
    
    // Read data in smaller chunks to be safer
    while (cur_len < total_len) {
        int chunk_size = (total_len - cur_len) > 256 ? 256 : (total_len - cur_len);
        int received = httpd_req_recv(req, content + cur_len, chunk_size);
        
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Failed to receive data: %d", received);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    content[cur_len] = '\0';

    ESP_LOGI(TAG, "Received WiFi config: %s", content);

    cJSON *json = cJSON_Parse(content);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    cJSON *password = cJSON_GetObjectItem(json, "password");
    
    if (!cJSON_IsString(ssid) || !cJSON_IsString(password)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID or password");
        return ESP_FAIL;
    }
    
    esp_err_t ret = wifi_manager_start_sta(ssid->valuestring, password->valuestring);
    cJSON_Delete(json);
    
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to configure WiFi");
        return ESP_FAIL;
    }
    
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

esp_err_t web_server_get_ota(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "OTA update page", strlen("OTA update page"));
    return ESP_OK;
}

esp_err_t web_server_post_ota(httpd_req_t *req)
{
    // TODO: Implement OTA update
    httpd_resp_send(req, "OTA update not implemented yet", strlen("OTA update not implemented yet"));
    return ESP_OK;
}

esp_err_t web_server_get_wifi_scan(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /wifi/scan");
    
    // Start WiFi scan
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 150
    };
    
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi scan: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start WiFi scan");
        return ESP_FAIL;
    }
    
    // Get scan results
    uint16_t number = 0;
    wifi_ap_record_t *ap_info = NULL;
    ret = esp_wifi_scan_get_ap_num(&number);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get scan count: %s", esp_err_to_name(ret));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get scan count");
        return ESP_FAIL;
    }
    
    if (number == 0) {
        // No networks found
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "[]", 1);
        return ESP_OK;
    }
    
    // Allocate memory for scan results
    ap_info = malloc(sizeof(wifi_ap_record_t) * number);
    if (ap_info == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for scan results");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate memory");
        return ESP_FAIL;
    }
    
    ret = esp_wifi_scan_get_ap_records(&number, ap_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get scan records: %s", esp_err_to_name(ret));
        free(ap_info);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get scan records");
        return ESP_FAIL;
    }
    
    // Create JSON response
    cJSON *json = cJSON_CreateArray();
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON array");
        free(ap_info);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create JSON");
        return ESP_FAIL;
    }
    
    for (int i = 0; i < number; i++) {
        cJSON *network = cJSON_CreateObject();
        if (network != NULL) {
            cJSON_AddStringToObject(network, "ssid", (char*)ap_info[i].ssid);
            cJSON_AddNumberToObject(network, "rssi", ap_info[i].rssi);
            cJSON_AddNumberToObject(network, "channel", ap_info[i].primary);
            cJSON_AddStringToObject(network, "security", 
                (ap_info[i].authmode == WIFI_AUTH_OPEN) ? "Open" : "Secured");
            cJSON_AddItemToArray(json, network);
        }
    }
    
    char *json_string = cJSON_Print(json);
    if (json_string != NULL) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_string, strlen(json_string));
        free(json_string);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to print JSON");
    }
    
    cJSON_Delete(json);
    free(ap_info);
    
    // Clear scan results
    esp_wifi_scan_stop();
    
    return ESP_OK;
}

esp_err_t web_server_post_mqtt(httpd_req_t *req)
{
    ESP_LOGI(TAG, "POST /mqtt");
    
    // Read request body
    char content[1024];
    int recv_len = httpd_req_recv(req, content, sizeof(content) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body");
        return ESP_FAIL;
    }
    content[recv_len] = '\0';
    
    ESP_LOGI(TAG, "Received MQTT config: %s", content);
    
    // Parse JSON
    cJSON *json = cJSON_Parse(content);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    // Extract MQTT configuration
    mqtt_config_t config;
    mqtt_config_get_default(&config); // Start with defaults
    
    cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
    if (cJSON_IsBool(enabled)) {
        config.enabled = cJSON_IsTrue(enabled);
    }
    
    cJSON *broker = cJSON_GetObjectItem(json, "broker");
    if (cJSON_IsString(broker) && broker->valuestring != NULL) {
        strncpy(config.broker, broker->valuestring, sizeof(config.broker) - 1);
        config.broker[sizeof(config.broker) - 1] = '\0';
    }
    
    cJSON *port = cJSON_GetObjectItem(json, "port");
    if (cJSON_IsNumber(port)) {
        config.port = port->valueint;
    }
    
    cJSON *username = cJSON_GetObjectItem(json, "username");
    if (cJSON_IsString(username) && username->valuestring != NULL) {
        strncpy(config.username, username->valuestring, sizeof(config.username) - 1);
        config.username[sizeof(config.username) - 1] = '\0';
    }
    
    cJSON *password = cJSON_GetObjectItem(json, "password");
    if (cJSON_IsString(password) && password->valuestring != NULL) {
        strncpy(config.password, password->valuestring, sizeof(config.password) - 1);
        config.password[sizeof(config.password) - 1] = '\0';
    }
    
    cJSON_Delete(json);
    
    // Save to NVS
    esp_err_t ret = mqtt_config_save(&config);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save MQTT config");
        return ret;
    }
    
    // Apply configuration
    ret = mqtt_config_apply(&config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply MQTT config");
    }
    
    // Send success response
    httpd_resp_set_type(req, "application/json");
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", true);
    cJSON_AddStringToObject(response, "message", "MQTT configuration saved");
    
    char *response_string = cJSON_Print(response);
    if (response_string != NULL) {
        httpd_resp_send(req, response_string, strlen(response_string));
        free(response_string);
    }
    
    cJSON_Delete(response);
    return ESP_OK;
}

esp_err_t web_server_init(void)
{
    ESP_LOGI(TAG, "Initializing web server");
    
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.max_uri_handlers = WEB_SERVER_MAX_URI_HANDLERS;
    
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server");
        return ret;
    }
    
    // Register URI handlers
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = web_server_get_root,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &root_uri);
    
    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = web_server_get_status,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &status_uri);
    
    httpd_uri_t relay_uri = {
        .uri = "/relay",
        .method = HTTP_POST,
        .handler = web_server_post_relay,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &relay_uri);
    
    httpd_uri_t wifi_uri = {
        .uri = "/wifi",
        .method = HTTP_POST,
        .handler = web_server_post_wifi,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wifi_uri);
    
    httpd_uri_t wifi_scan_uri = {
        .uri = "/wifi/scan",
        .method = HTTP_GET,
        .handler = web_server_get_wifi_scan,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &wifi_scan_uri);
    
    httpd_uri_t mqtt_uri = {
        .uri = "/mqtt",
        .method = HTTP_POST,
        .handler = web_server_post_mqtt,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &mqtt_uri);
    
    httpd_uri_t ota_uri = {
        .uri = "/ota",
        .method = HTTP_GET,
        .handler = web_server_get_ota,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_uri);
    
    httpd_uri_t ota_post_uri = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = web_server_post_ota,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &ota_post_uri);
    
    ESP_LOGI(TAG, "Web server initialized successfully");
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    if (server == NULL) {
        ESP_LOGE(TAG, "Web server not initialized");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Web server started on port %d", WEB_SERVER_PORT);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (server == NULL) {
        return ESP_FAIL;
    }
    
    esp_err_t ret = httpd_stop(server);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop web server");
        return ret;
    }
    
    server = NULL;
    ESP_LOGI(TAG, "Web server stopped");
    return ESP_OK;
}
