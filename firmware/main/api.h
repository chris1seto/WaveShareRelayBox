#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

/*
 * JSON views and updates shared by the HTTP API and the WebSocket API.
 * Builders return a new cJSON tree the caller must delete.
 */

/** Live state: relays, Wi-Fi, MQTT, Modbus, time, system. */
cJSON *api_status(void);

/** Settings (passwords omitted, only whether they are set). */
cJSON *api_config(void);

/** Apply any subset of the config fields. On failure returns false and fills `err`. */
bool api_config_update(const cJSON *body, char *err, size_t err_len);

/** {"schedules": [...]} */
cJSON *api_schedules(void);

/**
 * Replace all schedules. Accepts {"schedules": [...]} or a bare array. Each entry:
 *   {"enabled": true, "relays": [0, "pump"], "action": "on"|"off"|"toggle",
 *    "time": "HH:MM", "days": [0..6]}   (0 = Sunday)
 */
bool api_schedules_update(const cJSON *body, char *err, size_t err_len);

/** Relay command, e.g. {"0": true, "pump": "toggle"}. */
bool api_relays_command(const cJSON *body, char *err, size_t err_len);
