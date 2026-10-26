#include "web_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mqtt_link.h"
#include "net_services.h"
#include "ota.h"
#include "relays.h"
#include "settings.h"
#include "wifi_mgr.h"

static const char *TAG = "http";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;

#define MAX_JSON_BODY 2048

// ---------------------------------------------------------------------------
// Response helpers

static esp_err_t send_json(httpd_req_t *req, cJSON *obj, const char *status)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    if (status) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, o, status);
}

static esp_err_t send_ok(httpd_req_t *req, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    if (msg) {
        cJSON_AddStringToObject(o, "message", msg);
    }
    return send_json(req, o, NULL);
}

/** Read and parse a JSON body. Sends an error response and returns NULL on failure. */
static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_JSON_BODY) {
        send_error(req, "400 Bad Request", req->content_len ? "Body too large" : "Empty body");
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        send_error(req, "500 Internal Server Error", "Out of memory");
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            return NULL;   // connection is gone; nothing to reply to
        }
        got += r;
    }
    buf[got] = '\0';
    cJSON *obj = cJSON_Parse(buf);
    free(buf);
    if (!cJSON_IsObject(obj)) {
        cJSON_Delete(obj);
        send_error(req, "400 Bad Request", "Expected a JSON object");
        return NULL;
    }
    return obj;
}

// ---------------------------------------------------------------------------
// Authentication (HTTP Basic, password only)

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char *in, char *out, size_t out_len)
{
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *in && *in != '='; in++) {
        int v = b64_value(*in);
        if (v < 0) {
            return 0;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n + 1 >= out_len) {
                return 0;
            }
            out[n++] = (char)((acc >> bits) & 0xFF);
        }
    }
    out[n] = '\0';
    return n;
}

static bool equal_ct(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = la != lb;
    for (size_t i = 0; i < la; i++) {
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i % (lb ? lb : 1)];
    }
    return diff == 0;
}

static bool authorized(httpd_req_t *req)
{
    device_settings_t ds;
    settings_get(&ds);
    if (!ds.admin_password[0]) {
        return true;
    }
    char hdr[200], creds[150];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK ||
        strncmp(hdr, "Basic ", 6) != 0 || b64_decode(hdr + 6, creds, sizeof(creds)) == 0) {
        return false;
    }
    const char *colon = strchr(creds, ':');
    return colon && equal_ct(colon + 1, ds.admin_password);
}

static esp_err_t deny(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Relay Box\", charset=\"UTF-8\"");
    return httpd_resp_sendstr(req, "Authentication required");
}

#define REQUIRE_AUTH(req) do { if (!authorized(req)) return deny(req); } while (0)

// ---------------------------------------------------------------------------
// JSON builders

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

static cJSON *build_status(void)
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
    char built[40];
    snprintf(built, sizeof(built), "%s %s", app->date, app->time);
    cJSON_AddStringToObject(sys, "built", built);
    cJSON_AddNumberToObject(sys, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(sys, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(sys, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(sys, "reset_reason", reset_reason_str());
    cJSON_AddStringToObject(sys, "partition", esp_ota_get_running_partition()->label);
    cJSON_AddBoolToObject(sys, "pending_verify", ota_pending_verify());
    return root;
}

static cJSON *build_config(void)
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
    for (int i = 0; i < RELAY_COUNT; i++) {
        cJSON_AddItemToArray(ids, cJSON_CreateString(ds.relay_ids[i]));
        cJSON_AddItemToArray(boot, cJSON_CreateString(boot_mode_str(ds.relay_boot[i])));
    }
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
// Config update helpers

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
                    // Only the HTTP server task runs this, so a static buffer is safe.
                    static char msg[96];
                    snprintf(msg, sizeof(msg), "relay name: relays %d and %d would both get the id \"%s\"",
                             i + 1, j + 1, s->relay_ids[i]);
                    *bad = msg;
                    return false;
                }
            }
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

// ---------------------------------------------------------------------------
// Handlers

static esp_err_t h_index(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
}

static esp_err_t h_status(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return send_json(req, build_status(), NULL);
}

static esp_err_t h_config_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return send_json(req, build_config(), NULL);
}

static esp_err_t h_config_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    device_settings_t old, s;
    settings_get(&old);
    s = old;
    const char *bad = NULL;
    bool ok = parse_config(body, &s, &bad);
    cJSON_Delete(body);
    if (!ok) {
        char msg[160];
        snprintf(msg, sizeof(msg), "Invalid %s", bad ? bad : "value");
        return send_error(req, "400 Bad Request", msg);
    }
    esp_err_t err = settings_save(&s);
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
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
    if (host_changed || memcmp(&old.mqtt, &s.mqtt, sizeof(s.mqtt)) ||
        memcmp(old.relay_names, s.relay_names, sizeof(s.relay_names))) {
        mqtt_link_reload();
    }
    return send_json(req, build_config(), NULL);
}

static esp_err_t h_relays(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    esp_err_t err = relays_apply_json(body);
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return send_error(req, "400 Bad Request",
                          "Expected {\"<0-5>\": true|false|\"toggle\", ...}");
    }
    return send_json(req, build_status(), NULL);
}

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    wifi_ap_record_t *aps;
    uint16_t n;
    esp_err_t err = wifi_mgr_scan(&aps, &n);
    if (err != ESP_OK) {
        return send_error(req, "503 Service Unavailable", "Scan failed, try again");
    }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)aps[i].ssid;
        if (!ssid[0]) {
            continue;
        }
        // Keep only the strongest entry for each SSID (results are sorted by RSSI).
        bool dup = false;
        for (int j = 0; j < i && !dup; j++) {
            dup = strcmp(ssid, (const char *)aps[j].ssid) == 0;
        }
        if (dup) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddNumberToObject(o, "channel", aps[i].primary);
        cJSON_AddBoolToObject(o, "secure", aps[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    free(aps);
    return send_json(req, arr, NULL);
}

static esp_err_t h_wifi_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "ssid"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "password"));
    if (!pass) {
        pass = "";
    }
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (ssid && ssid[0] && strlen(ssid) <= 32 && strlen(pass) <= 64 &&
        (pass[0] == '\0' || strlen(pass) >= 8)) {
        err = wifi_mgr_set_credentials(ssid, pass);
    }
    cJSON_Delete(body);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, "400 Bad Request",
                          "SSID must be 1-32 characters; password empty or 8-64 characters");
    }
    if (err != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    device_settings_t ds;
    settings_get(&ds);
    char msg[128];
    snprintf(msg, sizeof(msg), "Connecting. If it succeeds the device will be at http://%s.local/",
             ds.hostname);
    return send_ok(req, msg);
}

static esp_err_t h_ota(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char msg[96];
    esp_err_t err = ota_from_request(req, msg, sizeof(msg));
    if (err != ESP_OK) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return send_error(req, err == ESP_ERR_NO_MEM || err == ESP_FAIL ? "500 Internal Server Error"
                                                                         : "400 Bad Request", msg);
    }
    send_ok(req, msg);
    ota_schedule_reboot(1500);
    return ESP_OK;
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    send_ok(req, "Rebooting");
    ota_schedule_reboot(1000);
    return ESP_OK;
}

static esp_err_t h_factory_reset(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    send_ok(req, "Settings erased, rebooting into setup mode");
    settings_factory_reset();
    ota_schedule_reboot(1000);
    return ESP_OK;
}

static esp_err_t h_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// ---------------------------------------------------------------------------

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",                  .method = HTTP_GET,  .handler = h_index },
        { .uri = "/favicon.ico",       .method = HTTP_GET,  .handler = h_favicon },
        { .uri = "/api/status",        .method = HTTP_GET,  .handler = h_status },
        { .uri = "/api/config",        .method = HTTP_GET,  .handler = h_config_get },
        { .uri = "/api/config",        .method = HTTP_POST, .handler = h_config_post },
        { .uri = "/api/relays",        .method = HTTP_POST, .handler = h_relays },
        { .uri = "/api/wifi/scan",     .method = HTTP_GET,  .handler = h_wifi_scan },
        { .uri = "/api/wifi",          .method = HTTP_POST, .handler = h_wifi_post },
        { .uri = "/api/ota",           .method = HTTP_POST, .handler = h_ota },
        { .uri = "/api/reboot",        .method = HTTP_POST, .handler = h_reboot },
        { .uri = "/api/factory-reset", .method = HTTP_POST, .handler = h_factory_reset },
        // v1.x compatibility
        { .uri = "/status",            .method = HTTP_GET,  .handler = h_status },
        { .uri = "/relay",             .method = HTTP_POST, .handler = h_relays },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &routes[i]));
    }
    ESP_LOGI(TAG, "Web server listening on port %d", cfg.server_port);
    return ESP_OK;
}

bool web_server_running(void)
{
    return s_server != NULL;
}
