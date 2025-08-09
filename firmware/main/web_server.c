#include "web_server.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "relay_control.h"
#include "wifi_manager.h"
#include "app_mqtt.h"
#include <string.h>

static const char *TAG = "WEB_SERVER";

static httpd_handle_t server = NULL;

// HTML for the main page
static const char* html_page =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "  <meta charset=\"UTF-8\" />\n"
    "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />\n"
    "  <title>Waveshare ESP32-S3 Relay</title>\n"
    "  <style>\n"
    "    :root{--bg:#0f1226;--card:#161a36;--accent:#6c8cff;--ok:#2ecc71;--err:#e74c3c;--muted:#95a5a6;}\n"
    "    *{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,\n"
    "    Cantarell,Noto Sans,sans-serif;background:linear-gradient(135deg,#0f1226,#11153a);color:#e9ecff;min-height:100vh}\n"
    "    .wrap{max-width:980px;margin:0 auto;padding:24px}\n"
    "    .title{display:flex;align-items:center;gap:12px;margin-bottom:16px}\n"
    "    .card{background:var(--card);border-radius:14px;padding:20px;margin:12px 0;box-shadow:0 6px 24px rgba(0,0,0,.25)}\n"
    "    .grid{display:grid;gap:16px}\n"
    "    @media(min-width:720px){.grid{grid-template-columns:1fr 1fr}}\n"
    "    .btn{appearance:none;border:0;background:var(--accent);color:#fff;padding:10px 14px;border-radius:10px;\n"
    "         font-weight:600;cursor:pointer;transition:.2s transform, .2s opacity}\n"
    "    .btn:hover{transform:translateY(-1px)}\n"
    "    .btn.secondary{background:#323760}\n"
    "    .btn.ok{background:var(--ok)} .btn.err{background:var(--err)}\n"
    "    .row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}\n"
    "    .field{display:flex;flex-direction:column;gap:6px;flex:1 1 220px}\n"
    "    input[type=text],input[type=password]{width:100%;padding:10px 12px;border-radius:10px;border:1px solid #2b2f5a;\n"
    "      background:#0f1230;color:#e9ecff;outline:none}input:focus{border-color:var(--accent)}\n"
    "    .badge{display:inline-flex;align-items:center;gap:6px;background:#212655;border:1px solid #2b2f5a;border-radius:999px;\n"
    "      padding:6px 10px;color:#c9d1ff;font-size:12px} .muted{color:var(--muted)}\n"
    "    .relay{display:flex;align-items:center;justify-content:space-between;padding:14px;border:1px solid #2b2f5a;\n"
    "      border-radius:12px;background:#111542}\n"
    "    .switch{position:relative;width:48px;height:28px;border-radius:999px;background:#444a80;border:1px solid #2b2f5a;\n"
    "      cursor:pointer;transition:background .2s}\n"
    "    .knob{position:absolute;top:2px;left:2px;width:23px;height:23px;border-radius:50%;background:#fff;transition:left .2s}\n"
    "    .on .switch{background:var(--ok)} .on .knob{left:23px}\n"
    "    .footer{margin-top:16px;font-size:12px;color:#aab2ff} code{background:#0d1030;padding:2px 6px;border-radius:6px}\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <div class=\"wrap\">\n"
    "    <div class=\"title\"><span style=\"font-size:22px\">🔌</span><h2 style=\"margin:0\">Waveshare ESP32-S3 Relay</h2><span class=\"badge\" id=\"netBadge\">Network: <span id=\"netMode\">Unknown</span></span></div>\n"
    "    <div class=\"grid\">\n"
    "      <div class=\"card\">\n"
    "        <h3 style=\"margin-top:0\">WiFi Configuration</h3>\n"
    "        <p class=\"muted\">Set SSID and password to connect in Station mode. Credentials are saved in NVS.</p>\n"
    "        <div class=\"row\">\n"
    "          <div class=\"field\"><label>SSID</label><input id=\"ssid\" type=\"text\" placeholder=\"WiFi SSID\" autocomplete=\"off\"></div>\n"
    "          <div class=\"field\"><label>Password</label><input id=\"password\" type=\"password\" placeholder=\"WiFi Password\"></div>\n"
    "        </div>\n"
    "        <div class=\"row\" style=\"margin-top:10px\">\n"
    "          <button class=\"btn\" onclick=\"saveWifi()\">Save & Connect</button>\n"
    "          <button class=\"btn secondary\" onclick=\"refreshStatus()\">Refresh Status</button>\n"
    "          <span id=\"wifiMsg\" class=\"muted\"></span>\n"
    "        </div>\n"
    "      </div>\n"
    "      <div class=\"card\">\n"
    "        <h3 style=\"margin-top:0\">Relay Control</h3>\n"
    "        <div id=\"relays\"></div>\n"
    "        <div class=\"row\" style=\"margin-top:10px\">\n"
    "          <button class=\"btn\" onclick=\"setAll(true)\">All ON</button>\n"
    "          <button class=\"btn err\" onclick=\"setAll(false)\">All OFF</button>\n"
    "        </div>\n"
    "      </div>\n"
    "    </div>\n"
    "    <div class=\"footer\">MQTT topics: <code>waveshare/relay/status</code>, <code>waveshare/relay/set</code></div>\n"
    "  </div>\n"
    "  <script>\n"
    "    let relayStates=[false,false,false,false,false,false];\n"
    "    function el(id){return document.getElementById(id)}\n"
    "    function relayRow(i){var on=relayStates[i] ? ' on' : '';return '\n"
    "      <div class=\"relay' + on + '\">' +\n"
    "        '<div style=\"font-weight:600\">Relay ' + (i+1) + '</div>' +\n"
    "        '<div class=\"switch\" onclick=\"toggle(' + i + ')\"><div class=\"knob\"></div></div>' +\n"
    "      '</div>'}\n"
    "    function draw(){const c=el('relays');c.innerHTML='';for(let i=0;i<6;i++){const d=document.createElement('div');d.innerHTML=relayRow(i);c.appendChild(d.firstChild);} }\n"
    "    async function refreshStatus(){try{const r=await fetch('/status');const s=await r.json();relayStates=[s.relays['0'],s.relays['1'],s.relays['2'],s.relays['3'],s.relays['4'],s.relays['5']];el('netMode').textContent=s.wifi_connected?('STA '+(s.ip_address||'')):'AP mode';draw();}catch(e){console.error(e);}}\n"
    "    async function toggle(i){const body={};body[i]=!relayStates[i];try{const r=await fetch('/relay',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});if(r.ok){relayStates[i]=!relayStates[i];draw();}}catch(e){console.error(e)}}\n"
    "    async function setAll(on){const body={0:on,1:on,2:on,3:on,4:on,5:on};try{const r=await fetch('/relay',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});if(r.ok){for(let i=0;i<6;i++) relayStates[i]=on;draw();}}catch(e){console.error(e)}}\n"
    "    async function saveWifi(){const ssid=el('ssid').value.trim();const password=el('password').value;el('wifiMsg').textContent='Saving...';try{const r=await fetch('/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,password})});el('wifiMsg').textContent=r.ok?'Saved. Connecting...':'Failed to save';if(r.ok){setTimeout(refreshStatus,3000)}}catch(e){console.error(e);el('wifiMsg').textContent='Failed'}}\n"
    "    refreshStatus();\n"
    "  </script>\n"
    "</body>\n"
    "</html>\n";

esp_err_t web_server_get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, strlen(html_page));
    return ESP_OK;
}

esp_err_t web_server_get_status(httpd_req_t *req)
{
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
    
    cJSON_AddBoolToObject(json, "mqtt_connected", false); // TODO: implement MQTT status
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
    char content[1024];
    int recv_len = httpd_req_recv(req, content, sizeof(content) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
        return ESP_FAIL;
    }
    content[recv_len] = '\0';
    
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
    char content[1024];
    int recv_len = httpd_req_recv(req, content, sizeof(content) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
        return ESP_FAIL;
    }
    content[recv_len] = '\0';
    
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
