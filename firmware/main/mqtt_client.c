#include "app_mqtt.h"
#include "esp_log.h"
#include "cJSON.h"
#include "relay_control.h"

static const char *TAG = "MQTT_CLIENT";

esp_mqtt_client_handle_t mqtt_client = NULL;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        
        // Subscribe to relay control topic
        char topic_set_conn[128];
        snprintf(topic_set_conn, sizeof(topic_set_conn), "%s%s", MQTT_TOPIC_ROOT, MQTT_TOPIC_SET);
        esp_mqtt_client_subscribe(client, topic_set_conn, 0);
        ESP_LOGI(TAG, "Subscribed to %s", topic_set_conn);
        
        // Publish initial status
        relay_publish_status();
        break;
        
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
        
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
        
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
        
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
        
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        ESP_LOGI(TAG, "TOPIC=%.*s\r\n", event->topic_len, event->topic);
        ESP_LOGI(TAG, "DATA=%.*s\r\n", event->data_len, event->data);
        
        // Check if this is a relay control message
        char topic_set[128];
        snprintf(topic_set, sizeof(topic_set), "%s%s", MQTT_TOPIC_ROOT, MQTT_TOPIC_SET);
        
        if (strncmp(event->topic, topic_set, event->topic_len) == 0) {
            // Create a null-terminated string from the data
            char *data = malloc(event->data_len + 1);
            if (data != NULL) {
                memcpy(data, event->data, event->data_len);
                data[event->data_len] = '\0';
                
                ESP_LOGI(TAG, "Processing relay control message: %s", data);
                
                // Parse and execute relay commands
                esp_err_t ret = relay_set_multiple(data);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to process relay control message");
                }
                
                free(data);
            }
        }
        break;
        
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
        
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

esp_err_t mqtt_client_init(void)
{
    ESP_LOGI(TAG, "Initializing MQTT client");
    
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URL,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
        .session.keepalive = 60,
        .session.disable_clean_session = false,
        .buffer.size = 1024,
        .buffer.out_size = 1024,
        .task.stack_size = 6144,
        .task.priority = 5,
    };
    
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT event handler");
        return err;
    }
    
    ESP_LOGI(TAG, "MQTT client initialized successfully");
    return ESP_OK;
}

esp_err_t mqtt_client_start(void)
{
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_mqtt_client_start(mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client");
        return err;
    }
    
    ESP_LOGI(TAG, "MQTT client started");
    return ESP_OK;
}

esp_err_t mqtt_client_stop(void)
{
    if (mqtt_client == NULL) {
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_mqtt_client_stop(mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop MQTT client");
        return err;
    }
    
    ESP_LOGI(TAG, "MQTT client stopped");
    return ESP_OK;
}

esp_err_t mqtt_publish_status(const char* status_json)
{
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return ESP_FAIL;
    }
    
    char topic_status[128];
    snprintf(topic_status, sizeof(topic_status), "%s%s", MQTT_TOPIC_ROOT, MQTT_TOPIC_STATUS);
    
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic_status, status_json, 0, 1, 0);
    if (msg_id == -1) {
        ESP_LOGE(TAG, "Failed to publish status");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Published status to %s: %s", topic_status, status_json);
    return ESP_OK;
}

esp_err_t mqtt_publish_config(const char* config_json)
{
    if (mqtt_client == NULL) {
        ESP_LOGE(TAG, "MQTT client not initialized");
        return ESP_FAIL;
    }
    
    char topic_config[128];
    snprintf(topic_config, sizeof(topic_config), "%s%s", MQTT_TOPIC_ROOT, MQTT_TOPIC_CONFIG);
    
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic_config, config_json, 0, 1, 0);
    if (msg_id == -1) {
        ESP_LOGE(TAG, "Failed to publish config");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Published config to %s: %s", topic_config, config_json);
    return ESP_OK;
}
