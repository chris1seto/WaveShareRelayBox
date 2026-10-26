#include "web_server.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "api.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ota.h"
#include "relays.h"
#include "settings.h"
#include "wifi_mgr.h"

static const char *TAG = "http";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;

#define MAX_JSON_BODY    4096
#define MAX_WS_MESSAGE   4096
#define MAX_CLIENTS      12
#define STATUS_PUSH_MS   5000

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
    if (!cJSON_IsObject(obj) && !cJSON_IsArray(obj)) {
        cJSON_Delete(obj);
        send_error(req, "400 Bad Request", "Expected JSON");
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

// ---------------------------------------------------------------------------
// Cross-site protection
//
// Browsers attach cookies/credentials but also say where a request came from. A page
// on another site must not be able to switch relays through a visitor's browser, so
// requests that change something are refused unless they come from our own page.
// Tools like curl send neither header and are allowed.

/** Copy a header into buf; returns 0 if absent, -1 if too long, else 1. */
static int get_header(httpd_req_t *req, const char *name, char *buf, size_t len)
{
    size_t n = httpd_req_get_hdr_value_len(req, name);
    if (n == 0) {
        return 0;
    }
    if (n >= len || httpd_req_get_hdr_value_str(req, name, buf, len) != ESP_OK) {
        return -1;
    }
    return 1;
}

static bool same_origin(httpd_req_t *req)
{
    char v[160], host[128];
    int r = get_header(req, "Sec-Fetch-Site", v, sizeof(v));
    if (r != 0) {
        // "none" = typed URL or bookmark
        return r > 0 && (!strcmp(v, "same-origin") || !strcmp(v, "none"));
    }
    r = get_header(req, "Origin", v, sizeof(v));
    if (r == 0) {
        return true;   // not from a browser page
    }
    if (r < 0 || get_header(req, "Host", host, sizeof(host)) <= 0) {
        return false;
    }
    const char *o = strstr(v, "://");
    return o && strcasecmp(o + 3, host) == 0;
}

static esp_err_t forbid(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Blocked cross-site request to %s", req->uri);
    return send_error(req, "403 Forbidden", "Cross-site request blocked");
}

#define REQUIRE_AUTH(req)  do { if (!authorized(req)) return deny(req); } while (0)
#define REQUIRE_WRITE(req) do { REQUIRE_AUTH(req); if (!same_origin(req)) return forbid(req); } while (0)

// ---------------------------------------------------------------------------
// WebSocket push
//
// Events are built and sent from the HTTP server task (via httpd_queue_work), so
// they never race with request handlers. Requests for the same event while one is
// pending are merged.

enum { EV_STATUS = 1, EV_CONFIG = 2, EV_SCHEDULES = 4 };
static atomic_uint s_pending_events;
static esp_timer_handle_t s_push_timer;

static int ws_clients(int *fds)
{
    size_t n = MAX_CLIENTS;
    int all[MAX_CLIENTS];
    if (!s_server || httpd_get_client_list(s_server, &n, all) != ESP_OK) {
        return 0;
    }
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(s_server, all[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            fds[count++] = all[i];
        }
    }
    return count;
}

static void ws_send_text(int fd, const char *s)
{
    httpd_ws_frame_t f = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)s,
        .len = strlen(s),
        .final = true,
    };
    httpd_ws_send_frame_async(s_server, fd, &f);
}

static char *event_json(const char *name, cJSON *data)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", name);
    cJSON_AddItemToObject(o, "data", data);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

static void push_events(int only_fd, unsigned events)
{
    int fds[MAX_CLIENTS];
    int n = only_fd >= 0 ? 1 : ws_clients(fds);
    if (only_fd >= 0) {
        fds[0] = only_fd;
    }
    if (n == 0) {
        return;
    }
    static const struct { unsigned bit; const char *name; cJSON *(*build)(void); } kinds[] = {
        { EV_CONFIG, "config", api_config },
        { EV_SCHEDULES, "schedules", api_schedules },
        { EV_STATUS, "status", api_status },
    };
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        if (!(events & kinds[k].bit)) {
            continue;
        }
        char *s = event_json(kinds[k].name, kinds[k].build());
        for (int i = 0; s && i < n; i++) {
            ws_send_text(fds[i], s);
        }
        free(s);
    }
}

static void push_work(void *arg)
{
    push_events(-1, atomic_exchange(&s_pending_events, 0));
}

static void notify(unsigned events)
{
    if (s_server && atomic_fetch_or(&s_pending_events, events) == 0) {
        if (httpd_queue_work(s_server, push_work, NULL) != ESP_OK) {
            atomic_store(&s_pending_events, 0);
        }
    }
}

static void on_relays_changed(uint32_t old_mask, uint32_t new_mask)
{
    notify(EV_STATUS);
}

static void push_timer_cb(void *arg)
{
    notify(EV_STATUS);   // RSSI, uptime, time, MQTT state...
}

// ---------------------------------------------------------------------------
// WebSocket API
//
// Request:  {"id": 7, "cmd": "set", "data": {"0": true, "pump": "toggle"}}
// Reply:    {"id": 7, "ok": true, "data": ...}  or  {"id": 7, "ok": false, "error": "..."}
// Events:   {"event": "status" | "config" | "schedules", "data": ...}
//
// Commands: ping, status, config, schedules, set, config_set, schedules_set

static void ws_reply(httpd_req_t *req, const cJSON *id, bool ok, cJSON *data, const char *error)
{
    cJSON *o = cJSON_CreateObject();
    if (id) {
        cJSON_AddItemToObject(o, "id", cJSON_Duplicate(id, true));
    }
    cJSON_AddBoolToObject(o, "ok", ok);
    if (data) {
        cJSON_AddItemToObject(o, "data", data);
    }
    if (error) {
        cJSON_AddStringToObject(o, "error", error);
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (s) {
        httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)s,
                               .len = strlen(s), .final = true };
        httpd_ws_send_frame(req, &f);
        free(s);
    }
}

static void ws_command(httpd_req_t *req, const char *text)
{
    cJSON *msg = cJSON_Parse(text);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(msg, "cmd"));
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(msg, "data");
    char err[160] = "";

    if (!cmd) {
        ws_reply(req, id, false, NULL, "Expected {\"cmd\": ..., \"data\": ...}");
    } else if (!strcmp(cmd, "ping")) {
        ws_reply(req, id, true, cJSON_CreateString("pong"), NULL);
    } else if (!strcmp(cmd, "status")) {
        ws_reply(req, id, true, api_status(), NULL);
    } else if (!strcmp(cmd, "config")) {
        ws_reply(req, id, true, api_config(), NULL);
    } else if (!strcmp(cmd, "schedules")) {
        ws_reply(req, id, true, api_schedules(), NULL);
    } else if (!strcmp(cmd, "set")) {
        bool ok = api_relays_command(data, err, sizeof(err));
        ws_reply(req, id, ok, NULL, ok ? NULL : err);   // the change itself arrives as an event
    } else if (!strcmp(cmd, "config_set")) {
        bool ok = api_config_update(data, err, sizeof(err));
        ws_reply(req, id, ok, ok ? api_config() : NULL, ok ? NULL : err);
        if (ok) notify(EV_CONFIG | EV_STATUS);
    } else if (!strcmp(cmd, "schedules_set")) {
        bool ok = api_schedules_update(data, err, sizeof(err));
        ws_reply(req, id, ok, ok ? api_schedules() : NULL, ok ? NULL : err);
        if (ok) notify(EV_SCHEDULES);
    } else {
        ws_reply(req, id, false, NULL, "Unknown cmd");
    }
    cJSON_Delete(msg);
}

static esp_err_t ws_pre_handshake(httpd_req_t *req)
{
    if (!same_origin(req)) {
        forbid(req);
        return ESP_FAIL;
    }
    if (!authorized(req)) {
        deny(req);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void hello_work(void *arg)
{
    push_events((int)(intptr_t)arg, EV_CONFIG | EV_SCHEDULES | EV_STATUS);
}

static esp_err_t ws_post_handshake(httpd_req_t *req)
{
    // Send the full picture to the new client once the handshake is complete.
    httpd_queue_work(req->handle, hello_work, (void *)(intptr_t)httpd_req_to_sockfd(req));
    return ESP_OK;
}

static esp_err_t h_ws(httpd_req_t *req)
{
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);   // learn the length
    if (err != ESP_OK) {
        return err;
    }
    if (f.len == 0 || f.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }
    if (f.len > MAX_WS_MESSAGE) {
        return ESP_FAIL;   // closes the connection
    }
    char *buf = malloc(f.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    f.payload = (uint8_t *)buf;
    err = httpd_ws_recv_frame(req, &f, f.len);
    if (err == ESP_OK) {
        buf[f.len] = '\0';
        ws_command(req, buf);
    }
    free(buf);
    return err;
}

// ---------------------------------------------------------------------------
// HTTP handlers

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
    return send_json(req, api_status(), NULL);
}

static esp_err_t h_config_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return send_json(req, api_config(), NULL);
}

static esp_err_t h_config_post(httpd_req_t *req)
{
    REQUIRE_WRITE(req);
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    char err[160];
    bool ok = api_config_update(body, err, sizeof(err));
    cJSON_Delete(body);
    if (!ok) {
        return send_error(req, "400 Bad Request", err);
    }
    notify(EV_CONFIG | EV_STATUS);
    return send_json(req, api_config(), NULL);
}

static esp_err_t h_schedules_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return send_json(req, api_schedules(), NULL);
}

static esp_err_t h_schedules_post(httpd_req_t *req)
{
    REQUIRE_WRITE(req);
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    char err[160];
    bool ok = api_schedules_update(body, err, sizeof(err));
    cJSON_Delete(body);
    if (!ok) {
        return send_error(req, "400 Bad Request", err);
    }
    notify(EV_SCHEDULES);
    return send_json(req, api_schedules(), NULL);
}

static esp_err_t h_relays(httpd_req_t *req)
{
    REQUIRE_WRITE(req);
    cJSON *body = read_json(req);
    if (!body) {
        return ESP_OK;
    }
    char err[160];
    bool ok = api_relays_command(body, err, sizeof(err));
    cJSON_Delete(body);
    if (!ok) {
        return send_error(req, "400 Bad Request", err);
    }
    return send_json(req, api_status(), NULL);
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
    REQUIRE_WRITE(req);
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
    REQUIRE_WRITE(req);
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
    REQUIRE_WRITE(req);
    send_ok(req, "Rebooting");
    ota_schedule_reboot(1000);
    return ESP_OK;
}

static esp_err_t h_factory_reset(httpd_req_t *req)
{
    REQUIRE_WRITE(req);
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
    cfg.max_uri_handlers = 20;
    cfg.max_open_sockets = MAX_CLIENTS;
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
        { .uri = "/api/schedules",     .method = HTTP_GET,  .handler = h_schedules_get },
        { .uri = "/api/schedules",     .method = HTTP_POST, .handler = h_schedules_post },
        { .uri = "/api/relays",        .method = HTTP_POST, .handler = h_relays },
        { .uri = "/api/wifi/scan",     .method = HTTP_GET,  .handler = h_wifi_scan },
        { .uri = "/api/wifi",          .method = HTTP_POST, .handler = h_wifi_post },
        { .uri = "/api/ota",           .method = HTTP_POST, .handler = h_ota },
        { .uri = "/api/reboot",        .method = HTTP_POST, .handler = h_reboot },
        { .uri = "/api/factory-reset", .method = HTTP_POST, .handler = h_factory_reset },
        { .uri = "/api/ws",            .method = HTTP_GET,  .handler = h_ws,
          .is_websocket = true,
          .ws_pre_handshake_cb = ws_pre_handshake,
          .ws_post_handshake_cb = ws_post_handshake },
        // v1.x compatibility
        { .uri = "/status",            .method = HTTP_GET,  .handler = h_status },
        { .uri = "/relay",             .method = HTTP_POST, .handler = h_relays },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &routes[i]));
    }

    relays_add_listener(on_relays_changed);
    const esp_timer_create_args_t ta = { .callback = push_timer_cb, .name = "ws_push" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_push_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_push_timer, STATUS_PUSH_MS * 1000ULL));

    ESP_LOGI(TAG, "Web server listening on port %d", cfg.server_port);
    return ESP_OK;
}

bool web_server_running(void)
{
    return s_server != NULL;
}
