#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * Start the HTTP server: the embedded web UI on "/" and a JSON API under /api.
 * If an admin password is set, every request needs HTTP Basic auth (any user name).
 *
 *   GET  /api/status          relays, network, MQTT, time, system info
 *   GET  /api/config          settings (passwords omitted)
 *   POST /api/config          partial settings update, applied immediately
 *   POST /api/relays          {"0":true,"3":"toggle",...}
 *   GET  /api/wifi/scan       nearby networks
 *   POST /api/wifi            {"ssid":"...","password":"..."}
 *   POST /api/ota             raw firmware .bin as the request body
 *   POST /api/reboot
 *   POST /api/factory-reset
 *
 * GET /status and POST /relay are kept for scripts written against firmware v1.x.
 */
esp_err_t web_server_start(void);

bool web_server_running(void);
