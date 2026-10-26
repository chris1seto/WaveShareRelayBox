#include "api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board_io.h"
#include "build_time.h"
#include "esp_app_desc.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "modbus.h"
#include "mqtt_link.h"
#include "net_services.h"
#include "ota.h"
#include "relays.h"
#include "settings.h"
#include "wifi_mgr.h"

static const char *TAG = "api";

// ---------------------------------------------------------------------------
// Status and config

static const char *boot_mode_str(relay_boot_mode_t m)
{
    switch (m) {
    case RELAY_BOOT_OFF: return "off";
    case RELAY_BOOT_ON: return "on";
    default: return "restore";
    }
}

static int parse_boot_mode(const char *v)
{
    if (!v) return -1;
    if (!strcmp(v, "off")) return RELAY_BOOT_OFF;
    if (!strcmp(v, "on")) return RELAY_BOOT_ON;
    if (!strcmp(v, "restore")) return RELAY_BOOT_RESTORE;
    return -1;
}

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_EXT: return "external";
    default: return "other";
    }
}

cJSON *api_status(void)
{
    device_settings_t ds;
    settings_get(&ds);
    wifi_mgr_status_t ws;
    wifi_mgr_get_status(&ws);
    uint32_t mask = relays_get_mask();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "hostname", ds.hostname);

    // "relays" keeps the v1.x shape for existing scripts.
    cJSON *relays = cJSON_AddObjectToObject(root, "relays");
    cJSON *list = cJSON_AddArrayToObject(root, "relay_list");
    for (int i = 0; i < RELAY_COUNT; i++) {
        char key[4];
        snprintf(key, sizeof(key), "%d", i);
        bool on = mask & (1u << i);
        cJSON_AddBoolToObject(relays, key, on);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "index", i);
        cJSON_AddStringToObject(r, "id", ds.relay_ids[i]);
        cJSON_AddStringToObject(r, "name", ds.relay_names[i]);
        cJSON_AddBoolToObject(r, "on", on);
        cJSON_AddStringToObject(r, "power_on", boot_mode_str(ds.relay_boot[i]));
        cJSON_AddNumberToObject(r, "group", ds.relay_group[i]);
        cJSON_AddItemToArray(list, r);
    }

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             ws.mac[0], ws.mac[1], ws.mac[2], ws.mac[3], ws.mac[4], ws.mac[5]);
    cJSON_AddBoolToObject(wifi, "configured", ws.has_credentials);
    cJSON_AddBoolToObject(wifi, "connected", ws.sta_connected);
    cJSON_AddStringToObject(wifi, "ssid", ws.sta_ssid);
    cJSON_AddStringToObject(wifi, "ip", ws.sta_ip);
    cJSON_AddNumberToObject(wifi, "rssi", ws.rssi);
    cJSON_AddStringToObject(wifi, "mac", mac);
    cJSON_AddBoolToObject(wifi, "ap_active", ws.ap_active);
    cJSON_AddStringToObject(wifi, "ap_ssid", ws.ap_ssid);
    cJSON_AddStringToObject(wifi, "ap_ip", ws.ap_ip);
    cJSON_AddNumberToObject(wifi, "ap_clients", ws.ap_clients);

    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    static const char *const mqtt_states[] = { "disabled", "connecting", "connected" };
    cJSON_AddBoolToObject(mqtt, "enabled", ds.mqtt.enabled);
    cJSON_AddStringToObject(mqtt, "state", mqtt_states[mqtt_link_state()]);

    cJSON *mb = cJSON_AddObjectToObject(root, "modbus");
    modbus_stats_t st;
    modbus_get_stats(&st);
    cJSON_AddBoolToObject(mb, "enabled", ds.hw.modbus_enabled);
    cJSON_AddNumberToObject(mb, "frames", st.frames_ok);
    cJSON_AddNumberToObject(mb, "crc_errors", st.crc_errors);
    cJSON_AddNumberToObject(mb, "exceptions", st.exceptions);

    cJSON *t = cJSON_AddObjectToObject(root, "time");
    char now[32];
    net_services_local_time(now, sizeof(now));
    cJSON_AddBoolToObject(t, "synced", net_services_time_synced());
    cJSON_AddStringToObject(t, "local", now);
    cJSON_AddStringToObject(t, "timezone", ds.timezone);

    cJSON *sys = cJSON_AddObjectToObject(root, "system");
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(sys, "version", app->version);
    cJSON_AddStringToObject(sys, "idf", esp_get_idf_version());
    cJSON_AddStringToObject(sys, "built", app_build_time);
    cJSON_AddNumberToObject(sys, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(sys, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(sys, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(sys, "reset_reason", reset_reason_str());
    cJSON_AddStringToObject(sys, "partition", esp_ota_get_running_partition()->label);
    cJSON_AddBoolToObject(sys, "pending_verify", ota_pending_verify());
    return root;
}

cJSON *api_config(void)
{
    device_settings_t ds;
    settings_get(&ds);
    wifi_mgr_status_t ws;
    wifi_mgr_get_status(&ws);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "hostname", ds.hostname);
    cJSON_AddStringToObject(root, "ntp_server", ds.ntp_server);
    cJSON_AddStringToObject(root, "timezone", ds.timezone);
    cJSON_AddBoolToObject(root, "admin_password_set", ds.admin_password[0] != '\0');
    cJSON_AddStringToObject(root, "wifi_ssid", ws.sta_ssid);
    cJSON *names = cJSON_AddArrayToObject(root, "relay_names");
    for (int i = 0; i < RELAY_COUNT; i++) {
        cJSON_AddItemToArray(names, cJSON_CreateString(ds.relay_names[i]));
    }
    cJSON *ids = cJSON_AddArrayToObject(root, "relay_ids");
    cJSON *boot = cJSON_AddArrayToObject(root, "relay_boot");
    cJSON *groups = cJSON_AddArrayToObject(root, "relay_groups");
    for (int i = 0; i < RELAY_COUNT; i++) {
        cJSON_AddItemToArray(ids, cJSON_CreateString(ds.relay_ids[i]));
        cJSON_AddItemToArray(boot, cJSON_CreateString(boot_mode_str(ds.relay_boot[i])));
        cJSON_AddItemToArray(groups, cJSON_CreateNumber(ds.relay_group[i]));
    }
    cJSON *hw = cJSON_AddObjectToObject(root, "hardware");
    cJSON_AddBoolToObject(hw, "status_led", ds.hw.status_led);
    cJSON_AddNumberToObject(hw, "led_brightness", ds.hw.led_brightness);
    cJSON_AddBoolToObject(hw, "buzzer", ds.hw.buzzer);
    cJSON_AddBoolToObject(hw, "modbus_enabled", ds.hw.modbus_enabled);
    cJSON_AddNumberToObject(hw, "modbus_address", ds.hw.modbus_address);
    cJSON_AddNumberToObject(hw, "modbus_baud", ds.hw.modbus_baud);
    cJSON *m = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddBoolToObject(m, "enabled", ds.mqtt.enabled);
    cJSON_AddStringToObject(m, "host", ds.mqtt.host);
    cJSON_AddNumberToObject(m, "port", ds.mqtt.port);
    cJSON_AddStringToObject(m, "username", ds.mqtt.username);
    cJSON_AddBoolToObject(m, "password_set", ds.mqtt.password[0] != '\0');
    cJSON_AddStringToObject(m, "root_topic", ds.mqtt.root_topic);
    cJSON_AddBoolToObject(m, "ha_discovery", ds.mqtt.ha_discovery);
    return root;
}

// ---------------------------------------------------------------------------
// Config updates

/** Copy an optional string field. Returns false (and sets *bad) if present but invalid. */
static bool take_str(const cJSON *obj, const char *key, char *dst, size_t len, const char **bad)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it) {
        return true;
    }
    if (!cJSON_IsString(it) || strlen(it->valuestring) >= len) {
        *bad = key;
        return false;
    }
    strlcpy(dst, it->valuestring, len);
    return true;
}

static bool take_bool(const cJSON *obj, const char *key, bool *dst, const char **bad)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it) {
        return true;
    }
    if (!cJSON_IsBool(it)) {
        *bad = key;
        return false;
    }
    *dst = cJSON_IsTrue(it);
    return true;
}

/** Optional integer field in [lo, hi], stored into either a u8 or a u32. */
static bool take_int(const cJSON *obj, const char *key, int lo, int hi, const char **bad,
                     uint8_t *dst8, uint32_t *dst32)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it) {
        return true;
    }
    if (!cJSON_IsNumber(it) || it->valuedouble < lo || it->valuedouble > hi) {
        *bad = key;
        return false;
    }
    if (dst8) *dst8 = (uint8_t)it->valueint;
    if (dst32) *dst32 = (uint32_t)it->valueint;
    return true;
}

static bool parse_config(const cJSON *body, device_settings_t *s, const char **bad)
{
    if (!take_str(body, "hostname", s->hostname, sizeof(s->hostname), bad) ||
        !take_str(body, "ntp_server", s->ntp_server, sizeof(s->ntp_server), bad) ||
        !take_str(body, "timezone", s->timezone, sizeof(s->timezone), bad) ||
        !take_str(body, "admin_password", s->admin_password, sizeof(s->admin_password), bad)) {
        return false;
    }
    for (char *p = s->hostname; *p; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
    if (!settings_hostname_valid(s->hostname)) {
        *bad = "hostname (use a-z, 0-9 and '-', up to 32 characters)";
        return false;
    }

    // relay_boot: one mode per relay, or a single string applied to all of them.
    const cJSON *boot = cJSON_GetObjectItemCaseSensitive(body, "relay_boot");
    if (boot) {
        bool all = cJSON_IsString(boot);
        if (!all && (!cJSON_IsArray(boot) || cJSON_GetArraySize(boot) != RELAY_COUNT)) {
            *bad = "relay_boot";
            return false;
        }
        for (int i = 0; i < RELAY_COUNT; i++) {
            int m = parse_boot_mode(cJSON_GetStringValue(all ? boot : cJSON_GetArrayItem(boot, i)));
            if (m < 0) {
                *bad = "relay_boot (use off, on or restore)";
                return false;
            }
            s->relay_boot[i] = (relay_boot_mode_t)m;
        }
    }

    // The relay id is derived from the name, so it is checked here rather than set.
    const cJSON *names = cJSON_GetObjectItemCaseSensitive(body, "relay_names");
    if (names) {
        if (!cJSON_IsArray(names) || cJSON_GetArraySize(names) != RELAY_COUNT) {
            *bad = "relay_names";
            return false;
        }
        for (int i = 0; i < RELAY_COUNT; i++) {
            const char *n = cJSON_GetStringValue(cJSON_GetArrayItem(names, i));
            if (!n || strlen(n) >= SETTINGS_NAME_LEN) {
                *bad = "relay name (up to 24 characters)";
                return false;
            }
            strlcpy(s->relay_names[i], n, SETTINGS_NAME_LEN);
            settings_relay_id_from_name(s->relay_names[i], s->relay_ids[i]);
            // Ids need a letter so MQTT can't confuse them with a numeric index.
            if (!strpbrk(s->relay_ids[i], "abcdefghijklmnopqrstuvwxyz")) {
                *bad = "relay name (needs at least one letter)";
                return false;
            }
        }
        for (int i = 0; i < RELAY_COUNT; i++) {
            for (int j = i + 1; j < RELAY_COUNT; j++) {
                if (strcmp(s->relay_ids[i], s->relay_ids[j]) == 0) {
                    // Only the HTTP server task runs this (HTTP and WebSocket), so a
                    // static buffer is safe.
                    static char msg[96];
                    snprintf(msg, sizeof(msg), "relay name: relays %d and %d would both get the id \"%s\"",
                             i + 1, j + 1, s->relay_ids[i]);
                    *bad = msg;
                    return false;
                }
            }
        }
    }

    const cJSON *groups = cJSON_GetObjectItemCaseSensitive(body, "relay_groups");
    if (groups) {
        if (!cJSON_IsArray(groups) || cJSON_GetArraySize(groups) != RELAY_COUNT) {
            *bad = "relay_groups";
            return false;
        }
        for (int i = 0; i < RELAY_COUNT; i++) {
            const cJSON *g = cJSON_GetArrayItem(groups, i);
            if (!cJSON_IsNumber(g) || g->valueint < 0 || g->valueint > RELAY_GROUPS) {
                *bad = "relay_groups (0 = none, 1-4)";
                return false;
            }
            s->relay_group[i] = (uint8_t)g->valueint;
        }
    }

    const cJSON *hw = cJSON_GetObjectItemCaseSensitive(body, "hardware");
    if (hw) {
        if (!cJSON_IsObject(hw) ||
            !take_bool(hw, "status_led", &s->hw.status_led, bad) ||
            !take_bool(hw, "buzzer", &s->hw.buzzer, bad) ||
            !take_bool(hw, "modbus_enabled", &s->hw.modbus_enabled, bad) ||
            !take_int(hw, "led_brightness", 1, 100, bad, &s->hw.led_brightness, NULL) ||
            !take_int(hw, "modbus_address", 1, 247, bad, &s->hw.modbus_address, NULL) ||
            !take_int(hw, "modbus_baud", 1200, 115200, bad, NULL, &s->hw.modbus_baud)) {
            if (!*bad) *bad = "hardware";
            return false;
        }
    }

    const cJSON *m = cJSON_GetObjectItemCaseSensitive(body, "mqtt");
    if (m) {
        if (!cJSON_IsObject(m) ||
            !take_bool(m, "enabled", &s->mqtt.enabled, bad) ||
            !take_bool(m, "ha_discovery", &s->mqtt.ha_discovery, bad) ||
            !take_str(m, "host", s->mqtt.host, sizeof(s->mqtt.host), bad) ||
            !take_str(m, "username", s->mqtt.username, sizeof(s->mqtt.username), bad) ||
            !take_str(m, "password", s->mqtt.password, sizeof(s->mqtt.password), bad) ||
            !take_str(m, "root_topic", s->mqtt.root_topic, sizeof(s->mqtt.root_topic), bad)) {
            if (!*bad) *bad = "mqtt";
            return false;
        }
        const cJSON *port = cJSON_GetObjectItemCaseSensitive(m, "port");
        if (port) {
            if (!cJSON_IsNumber(port) || port->valuedouble < 1 || port->valuedouble > 65535) {
                *bad = "mqtt.port";
                return false;
            }
            s->mqtt.port = (uint16_t)port->valueint;
        }
        if (s->mqtt.enabled && !s->mqtt.host[0]) {
            *bad = "mqtt.host (required when MQTT is enabled)";
            return false;
        }
        if (strpbrk(s->mqtt.root_topic, "#+") || !s->mqtt.root_topic[0]) {
            *bad = "mqtt.root_topic";
            return false;
        }
    }
    return true;
}

bool api_config_update(const cJSON *body, char *err, size_t err_len)
{
    if (!cJSON_IsObject(body)) {
        snprintf(err, err_len, "Expected a JSON object");
        return false;
    }
    device_settings_t old, s;
    settings_get(&old);
    s = old;
    const char *bad = NULL;
    if (!parse_config(body, &s, &bad)) {
        snprintf(err, err_len, "Invalid %s", bad ? bad : "value");
        return false;
    }
    esp_err_t e = settings_save(&s);
    if (e != ESP_OK) {
        snprintf(err, err_len, "Saving failed: %s", esp_err_to_name(e));
        return false;
    }

    bool host_changed = strcmp(old.hostname, s.hostname) != 0;
    if (host_changed) {
        ESP_LOGI(TAG, "Hostname -> %s", s.hostname);
        wifi_mgr_set_hostname(s.hostname);
        net_services_set_hostname(s.hostname);
    }
    if (strcmp(old.ntp_server, s.ntp_server) || strcmp(old.timezone, s.timezone)) {
        net_services_set_time_config(s.ntp_server, s.timezone);
    }
    if (memcmp(old.relay_group, s.relay_group, sizeof(s.relay_group))) {
        relays_set_groups(s.relay_group);
    }
    if (memcmp(&old.hw, &s.hw, sizeof(s.hw))) {
        board_io_reload();
        if (old.hw.modbus_enabled != s.hw.modbus_enabled ||
            old.hw.modbus_address != s.hw.modbus_address || old.hw.modbus_baud != s.hw.modbus_baud) {
            modbus_reload();
        }
    }
    if (host_changed || memcmp(&old.mqtt, &s.mqtt, sizeof(s.mqtt)) ||
        memcmp(old.relay_names, s.relay_names, sizeof(s.relay_names))) {
        mqtt_link_reload();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Schedules

static const char *const ACTIONS[] = { "off", "on", "toggle" };

cJSON *api_schedules(void)
{
    schedule_t list[SCHEDULE_MAX];
    int n = settings_get_schedules(list);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "schedules");
    for (int i = 0; i < n; i++) {
        const schedule_t *e = &list[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "enabled", e->enabled);
        cJSON *relays = cJSON_AddArrayToObject(o, "relays");
        for (int r = 0; r < RELAY_COUNT; r++) {
            if (e->relays & (1u << r)) {
                cJSON_AddItemToArray(relays, cJSON_CreateNumber(r));
            }
        }
        cJSON_AddStringToObject(o, "action", ACTIONS[e->action]);
        char t[8];
        snprintf(t, sizeof(t), "%02u:%02u", (unsigned)(e->hour % 24), (unsigned)(e->minute % 60));
        cJSON_AddStringToObject(o, "time", t);
        cJSON *days = cJSON_AddArrayToObject(o, "days");
        for (int d = 0; d < 7; d++) {
            if (e->days & (1u << d)) {
                cJSON_AddItemToArray(days, cJSON_CreateNumber(d));
            }
        }
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(root, "max", SCHEDULE_MAX);
    return root;
}

static bool parse_schedule(const cJSON *o, schedule_t *e, char *err, size_t err_len, int n)
{
    memset(e, 0, sizeof(*e));
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(o, "enabled");
    e->enabled = !en || cJSON_IsTrue(en);

    const cJSON *relays = cJSON_GetObjectItemCaseSensitive(o, "relays");
    const cJSON *it;
    if (!cJSON_IsArray(relays)) {
        snprintf(err, err_len, "Schedule %d: \"relays\" must be a list", n);
        return false;
    }
    cJSON_ArrayForEach(it, relays) {
        char tok[SETTINGS_ID_LEN];
        if (cJSON_IsNumber(it)) {
            snprintf(tok, sizeof(tok), "%d", it->valueint);
        } else if (cJSON_IsString(it)) {
            strlcpy(tok, it->valuestring, sizeof(tok));
        } else {
            tok[0] = '\0';
        }
        int idx = relays_find(tok);
        if (idx < 0) {
            snprintf(err, err_len, "Schedule %d: unknown relay", n);
            return false;
        }
        e->relays |= 1u << idx;
    }
    if (!e->relays) {
        snprintf(err, err_len, "Schedule %d: pick at least one relay", n);
        return false;
    }

    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "action"));
    e->action = 255;
    for (int a = 0; action && a < 3; a++) {
        if (!strcmp(action, ACTIONS[a])) {
            e->action = a;
        }
    }
    if (e->action == 255) {
        snprintf(err, err_len, "Schedule %d: action must be on, off or toggle", n);
        return false;
    }

    const char *t = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "time"));
    unsigned h, m;
    char extra;
    if (!t || sscanf(t, "%u:%u%c", &h, &m, &extra) != 2 || h > 23 || m > 59) {
        snprintf(err, err_len, "Schedule %d: time must be HH:MM", n);
        return false;
    }
    e->hour = h;
    e->minute = m;

    const cJSON *days = cJSON_GetObjectItemCaseSensitive(o, "days");
    if (!cJSON_IsArray(days)) {
        snprintf(err, err_len, "Schedule %d: \"days\" must be a list of 0-6 (0 = Sunday)", n);
        return false;
    }
    cJSON_ArrayForEach(it, days) {
        if (!cJSON_IsNumber(it) || it->valueint < 0 || it->valueint > 6) {
            snprintf(err, err_len, "Schedule %d: days are 0-6 (0 = Sunday)", n);
            return false;
        }
        e->days |= 1u << it->valueint;
    }
    if (!e->days) {
        snprintf(err, err_len, "Schedule %d: pick at least one day", n);
        return false;
    }
    return true;
}

bool api_schedules_update(const cJSON *body, char *err, size_t err_len)
{
    const cJSON *arr = cJSON_IsArray(body) ? body : cJSON_GetObjectItemCaseSensitive(body, "schedules");
    if (!cJSON_IsArray(arr)) {
        snprintf(err, err_len, "Expected {\"schedules\": [...]}");
        return false;
    }
    int n = cJSON_GetArraySize(arr);
    if (n > SCHEDULE_MAX) {
        snprintf(err, err_len, "At most %d schedules", SCHEDULE_MAX);
        return false;
    }
    schedule_t list[SCHEDULE_MAX];
    for (int i = 0; i < n; i++) {
        if (!parse_schedule(cJSON_GetArrayItem(arr, i), &list[i], err, err_len, i + 1)) {
            return false;
        }
    }
    esp_err_t e = settings_set_schedules(list, n);
    if (e != ESP_OK) {
        snprintf(err, err_len, "Saving failed: %s", esp_err_to_name(e));
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------

bool api_relays_command(const cJSON *body, char *err, size_t err_len)
{
    if (relays_apply_json(body) != ESP_OK) {
        snprintf(err, err_len, "Expected {\"<index or id>\": true|false|\"toggle\", ...}");
        return false;
    }
    return true;
}
