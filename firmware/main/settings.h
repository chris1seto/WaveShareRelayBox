#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define RELAY_COUNT 6

#define SETTINGS_HOSTNAME_LEN   33   // 32 chars + NUL
#define SETTINGS_NAME_LEN       25
#define SETTINGS_SSID_LEN       33
#define SETTINGS_WIFI_PASS_LEN  65

typedef enum {
    RELAY_BOOT_OFF = 0,
    RELAY_BOOT_ON = 1,
    RELAY_BOOT_RESTORE = 2,
} relay_boot_mode_t;

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
    relay_boot_mode_t relay_boot;
    char relay_names[RELAY_COUNT][SETTINGS_NAME_LEN];
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

/** Erase everything this firmware stores. The caller should reboot afterwards. */
esp_err_t settings_factory_reset(void);

/** true if the hostname is a valid DNS label (a-z, 0-9, '-', 1..32 chars). */
bool settings_hostname_valid(const char *hostname);

/** Returns a short id derived from the MAC, e.g. "a1b2c3". */
const char *settings_device_id(void);
