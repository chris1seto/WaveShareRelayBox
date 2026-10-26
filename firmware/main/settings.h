#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define RELAY_COUNT 6

#define SETTINGS_HOSTNAME_LEN   33   // 32 chars + NUL
#define SETTINGS_NAME_LEN       25
#define SETTINGS_ID_LEN         25   // relay id: [a-z0-9_-], 1..24 chars
#define SETTINGS_SSID_LEN       33
#define SETTINGS_WIFI_PASS_LEN  65

typedef enum {
    RELAY_BOOT_OFF = 0,
    RELAY_BOOT_ON = 1,
    RELAY_BOOT_RESTORE = 2,
} relay_boot_mode_t;

#define RELAY_GROUPS   4    // interlock groups A-D; 0 = not interlocked
#define SCHEDULE_MAX   24

typedef struct {
    bool status_led;          // RGB LED shows connection state
    uint8_t led_brightness;   // 1..100 %
    bool buzzer;              // short beep whenever a relay changes
    bool modbus_enabled;      // Modbus RTU slave on the RS485 port
    uint8_t modbus_address;   // 1..247
    uint32_t modbus_baud;     // 1200..115200, 8N1
} hw_settings_t;

typedef enum {
    SCHED_OFF = 0,
    SCHED_ON = 1,
    SCHED_TOGGLE = 2,
} sched_action_t;

typedef struct {
    bool enabled;
    uint8_t relays;    // bitmask of relays this entry switches
    uint8_t action;    // sched_action_t
    uint8_t days;      // bit 0 = Sunday ... bit 6 = Saturday
    uint8_t hour;      // local time
    uint8_t minute;
} schedule_t;

typedef struct {
    bool enabled;
    char host[96];          // hostname/IP, or a full mqtt:// / mqtts:// URI
    uint16_t port;
    char username[64];
    char password[64];
    char root_topic[64];    // stored without a trailing '/'
    bool ha_discovery;      // publish Home Assistant discovery configs
} mqtt_settings_t;

typedef struct {
    char hostname[SETTINGS_HOSTNAME_LEN];
    char ntp_server[64];
    char timezone[64];
    char admin_password[64];   // empty = web UI/API is open
    relay_boot_mode_t relay_boot[RELAY_COUNT];          // what each relay does after power loss
    char relay_names[RELAY_COUNT][SETTINGS_NAME_LEN];   // free-text display name
    char relay_ids[RELAY_COUNT][SETTINGS_ID_LEN];       // derived from the name, read-only
    uint8_t relay_group[RELAY_COUNT];                   // 0 = none, 1..RELAY_GROUPS = interlock group
    hw_settings_t hw;
    mqtt_settings_t mqtt;
} device_settings_t;

/** Load settings from NVS (with defaults and migration from firmware v1.x). Call after nvs_flash_init(). */
esp_err_t settings_init(void);

/** Copy the current settings. Thread-safe. */
void settings_get(device_settings_t *out);

/** Validate, persist and cache new settings. Thread-safe. */
esp_err_t settings_save(const device_settings_t *in);

/** Wi-Fi station credentials. Returns ESP_ERR_NOT_FOUND when none are saved. */
esp_err_t settings_get_wifi(char ssid[SETTINGS_SSID_LEN], char pass[SETTINGS_WIFI_PASS_LEN]);
esp_err_t settings_set_wifi(const char *ssid, const char *pass);

/** Last relay state bitmask, used by RELAY_BOOT_RESTORE. */
esp_err_t settings_get_relay_mask(uint32_t *mask);
esp_err_t settings_set_relay_mask(uint32_t mask);

/** Schedules (kept separately from device_settings_t because they are larger). */
int settings_get_schedules(schedule_t out[SCHEDULE_MAX]);
esp_err_t settings_set_schedules(const schedule_t *in, int count);

/** Erase everything this firmware stores. The caller should reboot afterwards. */
esp_err_t settings_factory_reset(void);

/** true if the hostname is a valid DNS label (a-z, 0-9, '-', 1..32 chars). */
bool settings_hostname_valid(const char *hostname);

/**
 * Derive a relay id from its display name: letters (lowercased), digits, '_' and '-' are kept,
 * runs of whitespace become one '_', everything else is dropped, max 24 chars.
 * The web UI mirrors this exactly to preview the id while typing.
 */
void settings_relay_id_from_name(const char *name, char out[SETTINGS_ID_LEN]);

/** Returns a short id derived from the MAC, e.g. "a1b2c3". */
const char *settings_device_id(void);
