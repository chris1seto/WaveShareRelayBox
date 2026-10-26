#include "relays.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "relays";

static const gpio_num_t s_gpios[RELAY_COUNT] = {
    CONFIG_RELAY_GPIO_1, CONFIG_RELAY_GPIO_2, CONFIG_RELAY_GPIO_3,
    CONFIG_RELAY_GPIO_4, CONFIG_RELAY_GPIO_5, CONFIG_RELAY_GPIO_6,
};

#define ALL_MASK ((1u << RELAY_COUNT) - 1)

static SemaphoreHandle_t s_lock;
static uint32_t s_mask;
static uint8_t s_groups[RELAY_COUNT];
static relays_listener_t s_listeners[6];
static int s_listener_count;

static inline int level_for(bool on)
{
#if CONFIG_RELAY_ACTIVE_LOW
    return on ? 0 : 1;
#else
    return on ? 1 : 0;
#endif
}

static void drive(uint32_t mask)
{
    for (int i = 0; i < RELAY_COUNT; i++) {
        gpio_set_level(s_gpios[i], level_for(mask & (1u << i)));
    }
}

/**
 * Enforce interlocks on a requested state. In each group at most one relay may be on:
 * prefer one that `requested_on` asks for, else keep whichever is already on, lowest
 * index first.
 */
static uint32_t apply_interlocks(uint32_t want, uint32_t requested_on)
{
    for (int g = 1; g <= RELAY_GROUPS; g++) {
        uint32_t members = 0;
        for (int i = 0; i < RELAY_COUNT; i++) {
            if (s_groups[i] == g) {
                members |= 1u << i;
            }
        }
        uint32_t on = want & members;
        if (on & (on - 1)) {   // more than one bit set
            uint32_t pick = (on & requested_on) ? (on & requested_on) : on;
            uint32_t keep = pick & -pick;   // lowest set bit
            want = (want & ~members) | keep;
        }
    }
    return want;
}

/** Drive `now`, switching interlocked relays off before their partners come on. */
static void drive_with_deadtime(uint32_t old, uint32_t now)
{
    uint32_t turning_off = old & ~now, turning_on = now & ~old;
    bool swap = false;
    for (int i = 0; i < RELAY_COUNT && !swap; i++) {
        for (int j = 0; j < RELAY_COUNT && !swap; j++) {
            swap = s_groups[i] && s_groups[i] == s_groups[j] &&
                   (turning_off & (1u << i)) && (turning_on & (1u << j));
        }
    }
    if (swap) {
        drive(old & now);   // everything that's switching off goes off first
        vTaskDelay(pdMS_TO_TICKS(CONFIG_RELAY_INTERLOCK_DEADTIME_MS));
    }
    drive(now);
}

esp_err_t relays_init(const relay_boot_mode_t boot_modes[RELAY_COUNT],
                      const uint8_t groups[RELAY_COUNT])
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t saved;
    if (settings_get_relay_mask(&saved) != ESP_OK) {
#if CONFIG_RELAY_FIRST_BOOT_ON
        saved = ALL_MASK;
#else
        saved = 0;
#endif
    }
    uint32_t mask = 0;
    for (int i = 0; i < RELAY_COUNT; i++) {
        bool on = boot_modes[i] == RELAY_BOOT_ON ||
                  (boot_modes[i] == RELAY_BOOT_RESTORE && (saved & (1u << i)));
        mask |= (uint32_t)on << i;
    }
    memcpy(s_groups, groups, sizeof(s_groups));
    s_mask = apply_interlocks(mask, 0);
    settings_set_relay_mask(s_mask);

    uint64_t pins = 0;
    for (int i = 0; i < RELAY_COUNT; i++) {
        pins |= 1ULL << s_gpios[i];
    }
    // Latch the output level before enabling the driver so relays don't chatter at boot.
    drive(s_mask);
    const gpio_config_t cfg = {
        .pin_bit_mask = pins,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    drive(s_mask);
    ESP_LOGI(TAG, "Initialised, state 0x%02" PRIx32, s_mask);
    return ESP_OK;
}

esp_err_t relays_set_mask(uint32_t which, uint32_t values)
{
    which &= ALL_MASK;   // which == 0 just re-applies interlocks
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t old = s_mask;
    uint32_t now = apply_interlocks((old & ~which) | (values & which), which & values);
    if (now != old) {
        s_mask = now;
        drive_with_deadtime(old, now);
        settings_set_relay_mask(now);   // skips the flash write if unchanged
    }
    relays_listener_t cbs[6];
    int n = s_listener_count;
    memcpy(cbs, s_listeners, sizeof(cbs));
    xSemaphoreGive(s_lock);

    if (now != old) {
        ESP_LOGI(TAG, "State 0x%02" PRIx32 " -> 0x%02" PRIx32, old, now);
        for (int i = 0; i < n; i++) {
            cbs[i](old, now);
        }
    }
    return ESP_OK;
}

esp_err_t relays_set(int idx, bool on)
{
    if (idx < 0 || idx >= RELAY_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return relays_set_mask(1u << idx, on ? (1u << idx) : 0);
}

esp_err_t relays_toggle(int idx)
{
    if (idx < 0 || idx >= RELAY_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    return relays_set(idx, !relays_get(idx));
}

uint32_t relays_get_mask(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t m = s_mask;
    xSemaphoreGive(s_lock);
    return m;
}

bool relays_get(int idx)
{
    return idx >= 0 && idx < RELAY_COUNT && (relays_get_mask() & (1u << idx));
}

void relays_add_listener(relays_listener_t cb)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_listener_count < (int)(sizeof(s_listeners) / sizeof(s_listeners[0]))) {
        s_listeners[s_listener_count++] = cb;
    }
    xSemaphoreGive(s_lock);
}

void relays_set_groups(const uint8_t groups[RELAY_COUNT])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_groups, groups, sizeof(s_groups));
    xSemaphoreGive(s_lock);
    // A new group may already contain two relays that are on.
    relays_set_mask(0, 0);
}

int relays_parse_payload(const char *s)
{
    if (!s) {
        return -1;
    }
    if (!strcasecmp(s, "on") || !strcasecmp(s, "true") || !strcmp(s, "1")) {
        return 1;
    }
    if (!strcasecmp(s, "off") || !strcasecmp(s, "false") || !strcmp(s, "0")) {
        return 0;
    }
    if (!strcasecmp(s, "toggle")) {
        return 2;
    }
    return -1;
}

int relays_find(const char *tok)
{
    if (!tok || !tok[0]) {
        return -1;
    }
    // All digits: an index. Ids always contain a letter, so this can't hide one.
    char *end = NULL;
    long idx = strtol(tok, &end, 10);
    if (end != tok && *end == '\0') {
        return (idx >= 0 && idx < RELAY_COUNT) ? (int)idx : -1;
    }
    device_settings_t ds;
    settings_get(&ds);
    for (int i = 0; i < RELAY_COUNT; i++) {
        if (strcasecmp(tok, ds.relay_ids[i]) == 0) {
            return i;
        }
    }
    return -1;
}

esp_err_t relays_apply_json(const cJSON *obj)
{
    if (!cJSON_IsObject(obj)) {
        return ESP_ERR_INVALID_ARG;
    }
    // Validate everything first so a bad key doesn't leave a half-applied command.
    uint32_t which = 0, values = 0, toggles = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, obj) {
        int idx = relays_find(item->string);
        if (idx < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        int v;
        if (cJSON_IsBool(item)) {
            v = cJSON_IsTrue(item);
        } else if (cJSON_IsNumber(item)) {
            v = item->valueint ? 1 : 0;
        } else if (cJSON_IsString(item)) {
            v = relays_parse_payload(item->valuestring);
        } else {
            v = -1;
        }
        if (v < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        if (v == 2) {
            toggles |= 1u << idx;
        } else {
            which |= 1u << idx;
            values |= (uint32_t)v << idx;
        }
    }
    if (toggles) {
        uint32_t cur = relays_get_mask();
        which |= toggles;
        values = (values & ~toggles) | (~cur & toggles);
    }
    return relays_set_mask(which, values);
}
