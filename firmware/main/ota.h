#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Stream a firmware image from an HTTP request body (application/octet-stream)
 * into the inactive OTA slot and make it the boot partition.
 * On failure `msg` holds a human-readable reason. The caller reboots on success.
 */
esp_err_t ota_from_request(httpd_req_t *req, char *msg, size_t msg_len);

/** true while the running image is waiting to be confirmed after an OTA. */
bool ota_pending_verify(void);

/** Confirm the running image so the bootloader won't roll it back. */
void ota_mark_valid(void);

/** Reboot after `delay_ms`, from a timer, so pending HTTP responses can finish. */
void ota_schedule_reboot(uint32_t delay_ms);
