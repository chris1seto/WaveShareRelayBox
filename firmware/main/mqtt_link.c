#include "mqtt_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "relays.h"
#include "settings.h"
#include "wifi_mgr.h"

static const char *TAG = "mqtt";

typedef enum {
    CMD_RELOAD,
    CMD_CONNECTED,       // publish everything after (re)connecting
    CMD_PUBLISH_STATE,
} cmd_t;

// The worker task owns the client: only it creates, destroys or publishes,
// so the MQTT event handler and HTTP handlers never touch the handle directly.
static QueueHandle_t s_queue;
static esp_mqtt_client_handle_t s_client;
static volatile mqtt_link_state_t s_state = MQTT_LINK_DISABLED;
static mqtt_settings_t s_cfg;
static char s_hostname[SETTINGS_HOSTNAME_LEN];
static char s_names[RELAY_COUNT][SETTINGS_NAME_LEN];
static char s_ids[RELAY_COUNT][SETTINGS_ID_LEN];
static char s_uri[128];
static char s_lwt_topic[96];

static void post(cmd_t cmd)
{
    if (s_queue) {
        xQueueSend(s_queue, &cmd, 0);
    }
}

// ---------------------------------------------------------------------------
// Publishing (worker task only)

static void publish(const char *suffix, const char *payload, bool retain)
{
    char topic[160];
    snprintf(topic, sizeof(topic), "%s/%s", s_cfg.root_topic, suffix);
    esp_mqtt_client_enqueue(s_client, topic, payload, 0, 1, retain, true);
}

static void publish_state(void)
{
    uint32_t mask = relays_get_mask();
    cJSON *obj = cJSON_CreateObject();
    for (int i = 0; i < RELAY_COUNT; i++) {
        char key[4], sub[48];
        const char *v = (mask & (1u << i)) ? "ON" : "OFF";
        snprintf(key, sizeof(key), "%d", i);
        cJSON_AddBoolToObject(obj, key, mask & (1u << i));
        // Same state under the index and the id, so either can be subscribed to.
        snprintf(sub, sizeof(sub), "relay/%d/state", i);
        publish(sub, v, true);
        snprintf(sub, sizeof(sub), "relay/%.24s/state", s_ids[i]);
        publish(sub, v, true);
    }
    char *json = cJSON_PrintUnformatted(obj);
    if (json) {
        publish("status", json, true);
        free(json);
    }
    cJSON_Delete(obj);
}

static void publish_info(void)
{
    wifi_mgr_status_t ws;
    wifi_mgr_get_status(&ws);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "hostname", s_hostname);
    cJSON_AddStringToObject(o, "ip", ws.sta_ip);
    cJSON_AddStringToObject(o, "version", esp_app_get_description()->version);
    cJSON *list = cJSON_AddArrayToObject(o, "relays");
    for (int i = 0; i < RELAY_COUNT; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "index", i);
        cJSON_AddStringToObject(r, "id", s_ids[i]);
        cJSON_AddStringToObject(r, "name", s_names[i]);
        cJSON_AddItemToArray(list, r);
    }
    char *json = cJSON_PrintUnformatted(o);
    if (json) {
        publish("info", json, true);
        free(json);
    }
    cJSON_Delete(o);
}

static void publish_ha_discovery(void)
{
    const char *id = settings_device_id();
    wifi_mgr_status_t ws;
    wifi_mgr_get_status(&ws);
    char url[48];
    snprintf(url, sizeof(url), "http://%s/", ws.sta_ip[0] ? ws.sta_ip : "");

    for (int i = 0; i < RELAY_COUNT; i++) {
        char buf[128];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", s_names[i]);
        snprintf(buf, sizeof(buf), "wsrelay_%s_relay%d", id, i);
        cJSON_AddStringToObject(o, "unique_id", buf);
        snprintf(buf, sizeof(buf), "%s/relay/%d/state", s_cfg.root_topic, i);
        cJSON_AddStringToObject(o, "state_topic", buf);
        snprintf(buf, sizeof(buf), "%s/relay/%d/set", s_cfg.root_topic, i);
        cJSON_AddStringToObject(o, "command_topic", buf);
        cJSON_AddStringToObject(o, "availability_topic", s_lwt_topic);
        cJSON_AddStringToObject(o, "payload_on", "ON");
        cJSON_AddStringToObject(o, "payload_off", "OFF");

        cJSON *dev = cJSON_AddObjectToObject(o, "device");
        cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
        snprintf(buf, sizeof(buf), "wsrelay_%s", id);
        cJSON_AddItemToArray(ids, cJSON_CreateString(buf));
        cJSON_AddStringToObject(dev, "name", s_hostname);
        cJSON_AddStringToObject(dev, "manufacturer", "Waveshare");
        cJSON_AddStringToObject(dev, "model", "ESP32-S3-Relay-6CH");
        cJSON_AddStringToObject(dev, "sw_version", esp_app_get_description()->version);
        if (ws.sta_ip[0]) {
            cJSON_AddStringToObject(dev, "configuration_url", url);
        }

        char *json = cJSON_PrintUnformatted(o);
        if (json) {
            char topic[96];
            snprintf(topic, sizeof(topic), "homeassistant/switch/wsrelay_%s/relay%d/config", id, i);
            esp_mqtt_client_enqueue(s_client, topic, json, 0, 1, 1, true);
            free(json);
        }
        cJSON_Delete(o);
    }
}

// ---------------------------------------------------------------------------
// Incoming messages (MQTT task)

static void handle_message(const char *topic, int topic_len, const char *data, int data_len)
{
    size_t root_len = strlen(s_cfg.root_topic);
    if (topic_len <= (int)root_len + 1 || strncmp(topic, s_cfg.root_topic, root_len) != 0 ||
        topic[root_len] != '/') {
        return;
    }
    const char *rest = topic + root_len + 1;
    int rest_len = topic_len - (int)root_len - 1;

    char payload[256];
    if (data_len >= (int)sizeof(payload)) {
        ESP_LOGW(TAG, "Payload too large (%d bytes)", data_len);
        return;
    }
    memcpy(payload, data, data_len);
    payload[data_len] = '\0';

    if (rest_len == 3 && memcmp(rest, "set", 3) == 0) {
        cJSON *obj = cJSON_Parse(payload);
        if (relays_apply_json(obj) != ESP_OK) {
            ESP_LOGW(TAG, "Bad command on %.*s: %s", topic_len, topic, payload);
        } else {
            ESP_LOGI(TAG, "set: %s", payload);
        }
        cJSON_Delete(obj);
        return;
    }

    // relay/<index or id>/set
    static const char prefix[] = "relay/", suffix[] = "/set";
    const int pre = sizeof(prefix) - 1, suf = sizeof(suffix) - 1;
    int tok_len = rest_len - pre - suf;
    if (tok_len <= 0 || tok_len >= SETTINGS_ID_LEN || memcmp(rest, prefix, pre) != 0 ||
        memcmp(rest + rest_len - suf, suffix, suf) != 0) {
        return;
    }
    char tok[SETTINGS_ID_LEN];
    memcpy(tok, rest + pre, tok_len);
    tok[tok_len] = '\0';
    int idx = relays_find(tok);
    if (idx < 0) {
        ESP_LOGW(TAG, "No relay '%s'", tok);
        return;
    }
    {
        ESP_LOGI(TAG, "relay/%s/set -> relay %d: %s", tok, idx, payload);
        int v = relays_parse_payload(payload);
        if (v == 2) {
            relays_toggle(idx);
        } else if (v >= 0) {
            relays_set(idx, v);
        } else {
            ESP_LOGW(TAG, "Bad payload for relay %d: %s", idx, payload);
        }
    }
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "Connected to %s", s_uri);
        s_state = MQTT_LINK_CONNECTED;
        char topic[160];
        snprintf(topic, sizeof(topic), "%s/set", s_cfg.root_topic);
        esp_mqtt_client_subscribe(ev->client, topic, 1);
        snprintf(topic, sizeof(topic), "%s/relay/+/set", s_cfg.root_topic);
        esp_mqtt_client_subscribe(ev->client, topic, 1);
        post(CMD_CONNECTED);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        if (s_state == MQTT_LINK_CONNECTED) {
            ESP_LOGW(TAG, "Disconnected");
        }
        s_state = MQTT_LINK_CONNECTING;
        break;
    case MQTT_EVENT_DATA:
        // Commands are tiny; ignore anything split across several events.
        if (ev->current_data_offset == 0 && ev->data_len == ev->total_data_len) {
            handle_message(ev->topic, ev->topic_len, ev->data, ev->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGW(TAG, "Connection refused (code %d) - check credentials",
                     ev->error_handle->connect_return_code);
        }
        break;
    default:
        break;
    }
}

static void on_relays_changed(uint32_t old_mask, uint32_t new_mask)
{
    post(CMD_PUBLISH_STATE);
}

// ---------------------------------------------------------------------------
// Client lifecycle (worker task only)

static void client_stop(void)
{
    if (s_client) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_state = MQTT_LINK_DISABLED;
}

static void client_start(void)
{
    device_settings_t ds;
    settings_get(&ds);
    s_cfg = ds.mqtt;
    strlcpy(s_hostname, ds.hostname, sizeof(s_hostname));
    memcpy(s_names, ds.relay_names, sizeof(s_names));
    memcpy(s_ids, ds.relay_ids, sizeof(s_ids));

    if (!s_cfg.enabled || !s_cfg.host[0]) {
        ESP_LOGI(TAG, "Disabled");
        return;
    }
    if (strstr(s_cfg.host, "://")) {
        strlcpy(s_uri, s_cfg.host, sizeof(s_uri));
    } else {
        snprintf(s_uri, sizeof(s_uri), "mqtt://%s:%u", s_cfg.host, s_cfg.port);
    }
    snprintf(s_lwt_topic, sizeof(s_lwt_topic), "%s/availability", s_cfg.root_topic);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_uri,
        .credentials.client_id = s_hostname,
        .credentials.username = s_cfg.username[0] ? s_cfg.username : NULL,
        .credentials.authentication.password = s_cfg.password[0] ? s_cfg.password : NULL,
        .session.keepalive = 30,
        .session.last_will = {
            .topic = s_lwt_topic,
            .msg = "offline",
            .qos = 1,
            .retain = 1,
        },
        .network.reconnect_timeout_ms = 5000,
        .buffer.size = 2048,
        .task.stack_size = 6144,
    };
    if (strncmp(s_uri, "mqtts://", 8) == 0 || strncmp(s_uri, "wss://", 6) == 0) {
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Client init failed (bad URI '%s'?)", s_uri);
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    s_state = MQTT_LINK_CONNECTING;
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        ESP_LOGE(TAG, "Client start failed");
        client_stop();
        return;
    }
    ESP_LOGI(TAG, "Connecting to %s as '%s', root topic '%s'", s_uri, s_hostname, s_cfg.root_topic);
}

// A renamed relay leaves a retained state message under its old id; clear it.
static void clear_renamed_ids(void)
{
    if (!s_client || s_state != MQTT_LINK_CONNECTED) {
        return;
    }
    device_settings_t ds;
    settings_get(&ds);
    if (strcmp(ds.mqtt.root_topic, s_cfg.root_topic) != 0) {
        return;   // topics are moving anyway
    }
    for (int i = 0; i < RELAY_COUNT; i++) {
        bool still_used = false;
        for (int j = 0; j < RELAY_COUNT; j++) {
            still_used |= strcmp(s_ids[i], ds.relay_ids[j]) == 0;
        }
        if (!still_used) {
            char topic[160];
            snprintf(topic, sizeof(topic), "%.63s/relay/%.24s/state", s_cfg.root_topic, s_ids[i]);
            // An empty retained message deletes the retained one on the broker.
            esp_mqtt_client_publish(s_client, topic, "", 0, 1, 1);
            ESP_LOGI(TAG, "Cleared %s", topic);
        }
    }
}

static void worker(void *arg)
{
    client_start();
    cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (cmd) {
        case CMD_RELOAD:
            clear_renamed_ids();
            client_stop();
            client_start();
            break;
        case CMD_CONNECTED:
            if (s_client) {
                publish("availability", "online", true);
                publish_info();
                if (s_cfg.ha_discovery) {
                    publish_ha_discovery();
                }
                publish_state();
            }
            break;
        case CMD_PUBLISH_STATE:
            if (s_client && s_state == MQTT_LINK_CONNECTED) {
                publish_state();
            }
            break;
        }
    }
}

esp_err_t mqtt_link_start(void)
{
    s_queue = xQueueCreate(8, sizeof(cmd_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }
    relays_set_listener(on_relays_changed);
    return xTaskCreate(worker, "mqtt_link", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void mqtt_link_reload(void)
{
    post(CMD_RELOAD);
}

mqtt_link_state_t mqtt_link_state(void)
{
    return s_state;
}
