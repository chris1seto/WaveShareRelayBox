#pragma once

#include <stdbool.h>
#include "esp_err.h"

/**
 * Start the HTTP server: the embedded web UI on "/", a JSON API under /api and a
 * WebSocket API on /api/ws. If an admin password is set, every request needs HTTP
 * Basic auth (any user name). Requests that change something are refused when a
 * browser says they come from another site (Origin / Sec-Fetch-Site headers).
 *
 *   GET  /api/status          relays, network, MQTT, Modbus, time, system info
 *   GET  /api/config          settings (passwords omitted)
 *   POST /api/config          partial settings update, applied immediately
 *   GET  /api/schedules       schedules
 *   POST /api/schedules       replace all schedules
 *   POST /api/relays          {"0": true, "pump": "toggle", ...}
 *   GET  /api/wifi/scan       nearby networks
 *   POST /api/wifi            {"ssid": "...", "password": "..."}
 *   POST /api/ota             raw firmware .bin as the request body
 *   POST /api/reboot
 *   POST /api/factory-reset
 *   GET  /api/ws              WebSocket: pushes status/config/schedules events and
 *                             accepts commands (see web_server.c)
 *
 * GET /status and POST /relay are kept for scripts written against firmware v1.x.
 */
esp_err_t web_server_start(void);

bool web_server_running(void);
