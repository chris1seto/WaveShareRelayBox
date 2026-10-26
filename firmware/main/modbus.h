#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * Modbus RTU slave on the RS485 port (8N1, address and baud rate from settings).
 *
 *   Coils 0-5            relays 1-6        FC 01 read, FC 05 write one, FC 15 write many
 *   Holding register 0   relay bitmask     FC 03 read, FC 06 write
 *
 * Writes go through the same path as the web UI and MQTT, so interlocks apply.
 * Address 0 is broadcast: writes are applied and no reply is sent.
 */
esp_err_t modbus_start(void);

/** Apply changed Modbus settings (enable, address, baud). */
void modbus_reload(void);

typedef struct {
    uint32_t frames_ok;       // requests addressed to us and answered/applied
    uint32_t crc_errors;
    uint32_t exceptions;      // requests answered with a Modbus exception
} modbus_stats_t;

void modbus_get_stats(modbus_stats_t *out);
