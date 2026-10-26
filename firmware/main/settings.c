#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

// Namespaces. "wifi_config" keeps the v1.x name and keys so existing devices stay connected.
#define NS_DEVICE "device"
#define NS_MQTT   "mqtt"
#define NS_WIFI   "wifi_config"
#define NS_LEGACY_MQTT "mqtt_config"   // v1.x stored MQTT as one struct blob here

static SemaphoreHandle_t s_lock;
static device_settings_t s_settings;
static schedule_t s_schedules[SCHEDULE_MAX];
static int s_schedule_count;
static char s_device_id[7];

// ---------------------------------------------------------------------------
// Helpers

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    snprintf(dst, dst_len, "%s", src ? src : "");
}

static void nvs_read_str(nvs_handle_t h, const char *key, char *out, size_t out_len)
{
    size_t len = out_len;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        // keep the default already in `out`
    }
}

static void normalize_topic(char *topic)
{
    size_t n = strlen(topic);
    while (n > 0 && topic[n - 1] == '/') {
        topic[--n] = '\0';
    }
}

bool settings_hostname_valid(const char *h)
{
    size_t n = h ? strlen(h) : 0;
    if (n == 0 || n > 32 || h[0] == '-' || h[n - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!(islower((unsigned char)c) || isdigit((unsigned char)c) || c == '-')) {
            return false;
        }
    }
    return true;
}

void settings_relay_id_from_name(const char *name, char out[SETTINGS_ID_LEN])
{
    const size_t max = SETTINGS_ID_LEN - 1;
    size_t n = 0;
    bool space = false;
    for (const char *p = name ? name : ""; *p && n < max; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x80 && (isalnum(c) || c == '_' || c == '-')) {
            if (space && n > 0 && n + 2 <= max) {
                out[n++] = '_';
            }
            space = false;
            out[n++] = (char)tolower(c);
        } else if (c < 0x80 && isspace(c)) {
            space = true;
        }
    }
    out[n] = '\0';
}

const char *settings_device_id(void)
{
    return s_device_id;
}

static void set_defaults(device_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->hostname, sizeof(s->hostname), "%.25s-%s", CONFIG_APP_HOSTNAME_PREFIX, s_device_id);
    copy_str(s->ntp_server, sizeof(s->ntp_server), CONFIG_APP_NTP_SERVER);
    copy_str(s->timezone, sizeof(s->timezone), CONFIG_APP_TIMEZONE);
    for (int i = 0; i < RELAY_COUNT; i++) {
        s->relay_boot[i] = RELAY_BOOT_RESTORE;
        snprintf(s->relay_names[i], SETTINGS_NAME_LEN, "Relay %d", i + 1);
    }
    s->hw.status_led = true;
    s->hw.led_brightness = 20;
    s->hw.buzzer = false;
    s->hw.modbus_enabled = false;
    s->hw.modbus_address = 1;
    s->hw.modbus_baud = 9600;
    s->mqtt.enabled = false;
    s->mqtt.port = 1883;
    s->mqtt.ha_discovery = true;
    copy_str(s->mqtt.root_topic, sizeof(s->mqtt.root_topic), CONFIG_APP_MQTT_DEFAULT_ROOT);
}

// ---------------------------------------------------------------------------
// v1.x migration

// Exact layout of the struct v1.x wrote with nvs_set_blob().
typedef struct {
    bool enabled;
    char broker[64];
    int port;
    char username[32];
    char password[64];
    char root_topic[64];   // missing in the earliest builds
} legacy_mqtt_blob_t;

static bool migrate_legacy_mqtt(mqtt_settings_t *m)
{
    nvs_handle_t h;
    if (nvs_open(NS_LEGACY_MQTT, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    legacy_mqtt_blob_t old = {0};
    size_t len = 0;
    bool ok = false;
    if (nvs_get_blob(h, "config", NULL, &len) == ESP_OK && len <= sizeof(old) &&
        len >= offsetof(legacy_mqtt_blob_t, root_topic) &&
        nvs_get_blob(h, "config", &old, &len) == ESP_OK) {
        m->enabled = old.enabled;
        copy_str(m->host, sizeof(m->host), old.broker);
        m->port = (old.port > 0 && old.port < 65536) ? (uint16_t)old.port : 1883;
        copy_str(m->username, sizeof(m->username), old.username);
        copy_str(m->password, sizeof(m->password), old.password);
        if (len == sizeof(old) && old.root_topic[0]) {
            old.root_topic[sizeof(old.root_topic) - 1] = '\0';
            copy_str(m->root_topic, sizeof(m->root_topic), old.root_topic);
        }
        ok = true;
        ESP_LOGI(TAG, "Migrated MQTT settings from firmware v1.x");
    }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    return ok;
}

// ---------------------------------------------------------------------------
// Load / save

static void load_all(device_settings_t *s)
{
    set_defaults(s);
    nvs_handle_t h;

    if (nvs_open(NS_DEVICE, NVS_READONLY, &h) == ESP_OK) {
        nvs_read_str(h, "hostname", s->hostname, sizeof(s->hostname));
        nvs_read_str(h, "ntp", s->ntp_server, sizeof(s->ntp_server));
        nvs_read_str(h, "tz", s->timezone, sizeof(s->timezone));
        nvs_read_str(h, "admin_pw", s->admin_password, sizeof(s->admin_password));
        // One "relay_boot" for all relays was used before per-relay settings existed.
        uint8_t boot = 0;
        if (nvs_get_u8(h, "relay_boot", &boot) == ESP_OK && boot <= RELAY_BOOT_RESTORE) {
            for (int i = 0; i < RELAY_COUNT; i++) {
                s->relay_boot[i] = (relay_boot_mode_t)boot;
            }
        }
        for (int i = 0; i < RELAY_COUNT; i++) {
            char key[8];
            snprintf(key, sizeof(key), "rname%d", i);
            nvs_read_str(h, key, s->relay_names[i], SETTINGS_NAME_LEN);
            snprintf(key, sizeof(key), "rboot%d", i);
            if (nvs_get_u8(h, key, &boot) == ESP_OK && boot <= RELAY_BOOT_RESTORE) {
                s->relay_boot[i] = (relay_boot_mode_t)boot;
            }
            snprintf(key, sizeof(key), "rgrp%d", i);
            uint8_t grp;
            if (nvs_get_u8(h, key, &grp) == ESP_OK && grp <= RELAY_GROUPS) {
                s->relay_group[i] = grp;
            }
        }
        uint8_t u8;
        uint32_t u32;
        if (nvs_get_u8(h, "led", &u8) == ESP_OK) s->hw.status_led = u8;
        if (nvs_get_u8(h, "led_bri", &u8) == ESP_OK && u8 >= 1 && u8 <= 100) s->hw.led_brightness = u8;
        if (nvs_get_u8(h, "buzzer", &u8) == ESP_OK) s->hw.buzzer = u8;
        if (nvs_get_u8(h, "mb_en", &u8) == ESP_OK) s->hw.modbus_enabled = u8;
        if (nvs_get_u8(h, "mb_addr", &u8) == ESP_OK && u8 >= 1 && u8 <= 247) s->hw.modbus_address = u8;
        if (nvs_get_u32(h, "mb_baud", &u32) == ESP_OK && u32 >= 1200 && u32 <= 115200) s->hw.modbus_baud = u32;
        nvs_close(h);
    }

    bool have_mqtt = false;
    if (nvs_open(NS_MQTT, NVS_READONLY, &h) == ESP_OK) {
        uint8_t u8;
        if (nvs_get_u8(h, "enabled", &u8) == ESP_OK) {
            have_mqtt = true;
            s->mqtt.enabled = u8;
        }
        if (nvs_get_u8(h, "ha", &u8) == ESP_OK) {
            s->mqtt.ha_discovery = u8;
        }
        uint16_t port;
        if (nvs_get_u16(h, "port", &port) == ESP_OK && port) {
            s->mqtt.port = port;
        }
        nvs_read_str(h, "host", s->mqtt.host, sizeof(s->mqtt.host));
        nvs_read_str(h, "user", s->mqtt.username, sizeof(s->mqtt.username));
        nvs_read_str(h, "pass", s->mqtt.password, sizeof(s->mqtt.password));
        nvs_read_str(h, "root", s->mqtt.root_topic, sizeof(s->mqtt.root_topic));
        nvs_close(h);
    }

    if (!have_mqtt && migrate_legacy_mqtt(&s->mqtt)) {
        normalize_topic(s->mqtt.root_topic);
        settings_save(s);   // s_lock exists; settings_save copies into s_settings
    }

    if (!settings_hostname_valid(s->hostname)) {
        ESP_LOGW(TAG, "Stored hostname invalid, using default");
        device_settings_t d;
        set_defaults(&d);
        copy_str(s->hostname, sizeof(s->hostname), d.hostname);
    }
    for (int i = 0; i < RELAY_COUNT; i++) {
        settings_relay_id_from_name(s->relay_names[i], s->relay_ids[i]);
    }
    normalize_topic(s->mqtt.root_topic);
    if (!s->mqtt.root_topic[0]) {
        copy_str(s->mqtt.root_topic, sizeof(s->mqtt.root_topic), CONFIG_APP_MQTT_DEFAULT_ROOT);
    }
}

// ---------------------------------------------------------------------------
// Schedules: stored as one blob of fixed 6-byte records after a version byte.

#define SCHED_BLOB_VERSION 1
#define SCHED_REC_SIZE     6

static bool schedule_valid(const schedule_t *e)
{
    return e->relays && !(e->relays & ~((1u << RELAY_COUNT) - 1)) && e->action <= SCHED_TOGGLE &&
           e->days && !(e->days & 0x80) && e->hour < 24 && e->minute < 60;
}

static void load_schedules(void)
{
    uint8_t blob[1 + SCHEDULE_MAX * SCHED_REC_SIZE];
    size_t len = sizeof(blob);
    nvs_handle_t h;
    int n = 0;
    if (nvs_open(NS_DEVICE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "sched", blob, &len) == ESP_OK && len >= 1 &&
            blob[0] == SCHED_BLOB_VERSION) {
            for (size_t off = 1; off + SCHED_REC_SIZE <= len && n < SCHEDULE_MAX; off += SCHED_REC_SIZE) {
                const uint8_t *r = blob + off;
                schedule_t e = {
                    .enabled = r[0] & 1, .action = (r[0] >> 1) & 3, .relays = r[1],
                    .days = r[2], .hour = r[3], .minute = r[4],
                };
                if (schedule_valid(&e)) {
                    s_schedules[n++] = e;
                }
            }
        }
        nvs_close(h);
    }
    s_schedule_count = n;
}

int settings_get_schedules(schedule_t out[SCHEDULE_MAX])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_schedule_count;
    memcpy(out, s_schedules, sizeof(schedule_t) * n);
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t settings_set_schedules(const schedule_t *in, int count)
{
    if (count < 0 || count > SCHEDULE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t blob[1 + SCHEDULE_MAX * SCHED_REC_SIZE] = { SCHED_BLOB_VERSION };
    for (int i = 0; i < count; i++) {
        if (!schedule_valid(&in[i])) {
            return ESP_ERR_INVALID_ARG;
        }
        uint8_t *r = blob + 1 + i * SCHED_REC_SIZE;
        r[0] = (uint8_t)(in[i].enabled | (in[i].action << 1));
        r[1] = in[i].relays;
        r[2] = in[i].days;
        r[3] = in[i].hour;
        r[4] = in[i].minute;
        r[5] = 0;   // reserved
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_DEVICE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "sched", blob, 1 + count * SCHED_REC_SIZE);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        memcpy(s_schedules, in, sizeof(schedule_t) * count);
        s_schedule_count = count;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t settings_init(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x", mac[3], mac[4], mac[5]);

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    device_settings_t s;
    load_all(&s);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_settings = s;
    xSemaphoreGive(s_lock);
    load_schedules();
    ESP_LOGI(TAG, "Hostname: %s, %d schedule(s)", s.hostname, s_schedule_count);
    return ESP_OK;
}

void settings_get(device_settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_settings;
    xSemaphoreGive(s_lock);
}

#define CHECK(x) do { err = (x); if (err != ESP_OK) goto done; } while (0)

esp_err_t settings_save(const device_settings_t *in)
{
    if (!settings_hostname_valid(in->hostname) || in->mqtt.port == 0 ||
        in->hw.led_brightness < 1 || in->hw.led_brightness > 100 ||
        in->hw.modbus_address < 1 || in->hw.modbus_address > 247 ||
        in->hw.modbus_baud < 1200 || in->hw.modbus_baud > 115200) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < RELAY_COUNT; i++) {
        if (in->relay_boot[i] > RELAY_BOOT_RESTORE || in->relay_group[i] > RELAY_GROUPS) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    device_settings_t s = *in;
    for (int i = 0; i < RELAY_COUNT; i++) {
        settings_relay_id_from_name(s.relay_names[i], s.relay_ids[i]);
    }
    normalize_topic(s.mqtt.root_topic);
    if (!s.mqtt.root_topic[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err;
    nvs_handle_t h = 0;

    CHECK(nvs_open(NS_DEVICE, NVS_READWRITE, &h));
    CHECK(nvs_set_str(h, "hostname", s.hostname));
    CHECK(nvs_set_str(h, "ntp", s.ntp_server));
    CHECK(nvs_set_str(h, "tz", s.timezone));
    CHECK(nvs_set_str(h, "admin_pw", s.admin_password));
    for (int i = 0; i < RELAY_COUNT; i++) {
        char key[8];
        snprintf(key, sizeof(key), "rname%d", i);
        CHECK(nvs_set_str(h, key, s.relay_names[i]));
        snprintf(key, sizeof(key), "rboot%d", i);
        CHECK(nvs_set_u8(h, key, (uint8_t)s.relay_boot[i]));
        snprintf(key, sizeof(key), "rgrp%d", i);
        CHECK(nvs_set_u8(h, key, s.relay_group[i]));
    }
    CHECK(nvs_set_u8(h, "led", s.hw.status_led));
    CHECK(nvs_set_u8(h, "led_bri", s.hw.led_brightness));
    CHECK(nvs_set_u8(h, "buzzer", s.hw.buzzer));
    CHECK(nvs_set_u8(h, "mb_en", s.hw.modbus_enabled));
    CHECK(nvs_set_u8(h, "mb_addr", s.hw.modbus_address));
    CHECK(nvs_set_u32(h, "mb_baud", s.hw.modbus_baud));
    CHECK(nvs_commit(h));
    nvs_close(h);
    h = 0;

    CHECK(nvs_open(NS_MQTT, NVS_READWRITE, &h));
    CHECK(nvs_set_u8(h, "enabled", s.mqtt.enabled));
    CHECK(nvs_set_u8(h, "ha", s.mqtt.ha_discovery));
    CHECK(nvs_set_u16(h, "port", s.mqtt.port));
    CHECK(nvs_set_str(h, "host", s.mqtt.host));
    CHECK(nvs_set_str(h, "user", s.mqtt.username));
    CHECK(nvs_set_str(h, "pass", s.mqtt.password));
    CHECK(nvs_set_str(h, "root", s.mqtt.root_topic));
    CHECK(nvs_commit(h));

    s_settings = s;
done:
    if (h) {
        nvs_close(h);
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving settings failed: %s", esp_err_to_name(err));
    }
    return err;
}

// ---------------------------------------------------------------------------
// Wi-Fi credentials and relay state

esp_err_t settings_get_wifi(char ssid[SETTINGS_SSID_LEN], char pass[SETTINGS_WIFI_PASS_LEN])
{
    ssid[0] = pass[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t len = SETTINGS_SSID_LEN;
    err = nvs_get_str(h, "ssid", ssid, &len);
    if (err == ESP_OK) {
        len = SETTINGS_WIFI_PASS_LEN;
        if (nvs_get_str(h, "password", pass, &len) != ESP_OK) {
            pass[0] = '\0';
        }
    }
    nvs_close(h);
    return (err == ESP_OK && ssid[0]) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t settings_set_wifi(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0] || strlen(ssid) >= SETTINGS_SSID_LEN || !pass ||
        strlen(pass) >= SETTINGS_WIFI_PASS_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "password", pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t settings_get_relay_mask(uint32_t *mask)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_DEVICE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_u32(h, "relay_mask", mask);
        nvs_close(h);
    }
    return err;
}

esp_err_t settings_set_relay_mask(uint32_t mask)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_DEVICE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        uint32_t old;
        // Skip the flash write if nothing changed
        if (nvs_get_u32(h, "relay_mask", &old) != ESP_OK || old != mask) {
            err = nvs_set_u32(h, "relay_mask", mask);
            if (err == ESP_OK) {
                err = nvs_commit(h);
            }
        }
        nvs_close(h);
    }
    return err;
}

esp_err_t settings_factory_reset(void)
{
    ESP_LOGW(TAG, "Factory reset: erasing NVS");
    return nvs_flash_erase();
}
