#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

typedef struct {
    bool has_credentials;
    bool sta_connected;
    char sta_ssid[33];
    char sta_ip[16];
    int8_t rssi;
    uint8_t mac[6];
    bool ap_active;
    char ap_ssid[33];
    char ap_ip[16];
    uint8_t ap_clients;
} wifi_mgr_status_t;

/**
 * Create netifs and start Wi-Fi. Connects with saved credentials (retrying forever
 * with backoff). If there are no credentials, or no connection for
 * CONFIG_APP_AP_FALLBACK_DELAY_S, a setup access point is started next to the
 * station; it is turned off again a while after the station connects.
 * Requires esp_netif_init() and the default event loop.
 */
esp_err_t wifi_mgr_start(const char *hostname);

/** Save new credentials and reconnect. Returns immediately. */
esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *password);

/** Change the DHCP hostname. Reconnects if currently connected so DHCP picks it up. */
esp_err_t wifi_mgr_set_hostname(const char *hostname);

void wifi_mgr_get_status(wifi_mgr_status_t *out);

/** Blocking scan. On success the caller must free(*records). */
esp_err_t wifi_mgr_scan(wifi_ap_record_t **records, uint16_t *count);
