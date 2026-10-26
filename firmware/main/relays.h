#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "settings.h"

struct cJSON;

/** Called after any relay changes, from the task that made the change. */
typedef void (*relays_listener_t)(uint32_t old_mask, uint32_t new_mask);

/** Configure GPIOs and apply the power-on state. */
esp_err_t relays_init(relay_boot_mode_t boot_mode);

/** Change the power-on behaviour at runtime. */
void relays_set_boot_mode(relay_boot_mode_t boot_mode);

esp_err_t relays_set(int idx, bool on);
esp_err_t relays_toggle(int idx);

/** Set every relay whose bit is in `which` to the matching bit of `values`, atomically. */
esp_err_t relays_set_mask(uint32_t which, uint32_t values);

bool relays_get(int idx);
uint32_t relays_get_mask(void);

void relays_set_listener(relays_listener_t cb);

/**
 * Apply a JSON command. Accepts an object whose keys are relay indexes ("0".."5")
 * and whose values are true/false, "on"/"off" or "toggle". Unknown keys are an error.
 */
esp_err_t relays_apply_json(const struct cJSON *obj);

/** Parse a payload like ON/OFF/TOGGLE/true/false/1/0. Returns -1 if invalid, 2 for toggle. */
int relays_parse_payload(const char *s);
