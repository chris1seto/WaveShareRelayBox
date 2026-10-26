#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "settings.h"

struct cJSON;

/** Called after any relay changes, from the task that made the change. */
typedef void (*relays_listener_t)(uint32_t old_mask, uint32_t new_mask);

/**
 * Configure GPIOs and apply each relay's power-on mode. The current state is saved
 * whenever it changes, so RELAY_BOOT_RESTORE always has the last state to go back to.
 * `groups` gives each relay's interlock group (0 = none).
 */
esp_err_t relays_init(const relay_boot_mode_t boot_modes[RELAY_COUNT],
                      const uint8_t groups[RELAY_COUNT]);

/**
 * Change interlock groups at runtime. Relays in the same group are never on together:
 * turning one on turns the others off first, with CONFIG_RELAY_INTERLOCK_DEADTIME_MS
 * between the two so a motor never sees both directions at once. If a command (or the
 * power-on state) asks for several relays of one group, the lowest-numbered one wins.
 */
void relays_set_groups(const uint8_t groups[RELAY_COUNT]);

esp_err_t relays_set(int idx, bool on);
esp_err_t relays_toggle(int idx);

/** Set every relay whose bit is in `which` to the matching bit of `values`, atomically
 *  (subject to interlocks). */
esp_err_t relays_set_mask(uint32_t which, uint32_t values);

bool relays_get(int idx);
uint32_t relays_get_mask(void);

/** Register a change listener (up to 6). Listeners run in the task that made the change. */
void relays_add_listener(relays_listener_t cb);

/**
 * Look up a relay by numeric index ("0".."5") or by its id (case-insensitive).
 * Returns the index, or -1 if nothing matches.
 */
int relays_find(const char *index_or_id);

/**
 * Apply a JSON command. Keys are relay indexes ("0".."5") or relay ids; values are
 * true/false, 1/0, "on"/"off" or "toggle". Unknown keys are an error.
 */
esp_err_t relays_apply_json(const struct cJSON *obj);

/** Parse a payload like ON/OFF/TOGGLE/true/false/1/0. Returns -1 if invalid, 2 for toggle. */
int relays_parse_payload(const char *s);
