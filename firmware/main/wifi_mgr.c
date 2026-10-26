#include "wifi_mgr.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "settings.h"

static const char *TAG = "wifi";

#define RECONNECT_MIN_MS   1000
#define RECONNECT_MAX_MS   60000
#define AP_LINGER_S        120     // keep the setup AP this long after STA connects

static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static SemaphoreHandle_t s_lock;     // protects the fields below
static esp_timer_handle_t s_reconnect_timer;
static esp_timer_handle_t s_ap_timer;

static bool s_has_creds;
static bool s_connected;
static bool s_ap_on;
static bool s_scanning;
static uint8_t s_ap_clients;
static uint32_t s_backoff_ms = RECONNECT_MIN_MS;
static char s_ssid[33];
static char s_ap_ssid[33];
static esp_ip4_addr_t s_ip;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

// ---------------------------------------------------------------------------
// Setup access point

static void ap_enable(bool on)
{
    LOCK();
    bool changed = (s_ap_on != on);
    s_ap_on = on;
    if (!on) {
        s_ap_clients = 0;
    }
    UNLOCK();
    if (!changed) {
        return;
    }
    if (on) {
        ESP_LOGI(TAG, "Starting setup AP '%s'", s_ap_ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        wifi_config_t ap = {
            .ap = {
                .channel = 1,
                .max_connection = 4,
                .authmode = WIFI_AUTH_WPA2_PSK,
                .pmf_cfg = { .required = false },
            },
        };
        strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
        ap.ap.ssid_len = strlen(s_ap_ssid);
        strlcpy((char *)ap.ap.password, CONFIG_APP_AP_PASSWORD, sizeof(ap.ap.password));
        if (strlen(CONFIG_APP_AP_PASSWORD) < 8) {
            ap.ap.authmode = WIFI_AUTH_OPEN;
        }
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    } else {
        ESP_LOGI(TAG, "Stopping setup AP");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }
}

static void ap_timer_cb(void *arg)
{
    LOCK();
    bool connected = s_connected, ap_on = s_ap_on, clients = s_ap_clients > 0;
    UNLOCK();

    if (!connected && !ap_on) {
        ap_enable(true);
    } else if (connected && ap_on) {
        if (clients) {
            // Someone is still using the setup AP; check again later.
            esp_timer_start_once(s_ap_timer, AP_LINGER_S * 1000000ULL);
        } else {
            ap_enable(false);
        }
    }
}

static void ap_timer_arm(uint32_t seconds)
{
    esp_timer_stop(s_ap_timer);
    esp_timer_start_once(s_ap_timer, (uint64_t)seconds * 1000000ULL);
}

// ---------------------------------------------------------------------------
// Station reconnect with exponential backoff

static void reconnect_timer_cb(void *arg)
{
    LOCK();
    bool go = s_has_creds && !s_connected && !s_scanning;
    UNLOCK();
    if (go) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        }
    }
}

static void schedule_reconnect(void)
{
    LOCK();
    uint32_t delay = s_backoff_ms;
    s_backoff_ms = s_backoff_ms * 2 > RECONNECT_MAX_MS ? RECONNECT_MAX_MS : s_backoff_ms * 2;
    UNLOCK();
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay * 1000ULL);
    ESP_LOGI(TAG, "Reconnecting in %" PRIu32 " ms", delay);
}

// ---------------------------------------------------------------------------
// Events

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START: {
        LOCK();
        bool creds = s_has_creds;
        UNLOCK();
        if (creds) {
            esp_wifi_connect();
        }
        break;
    }

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ev = data;
        LOCK();
        bool was_connected = s_connected;
        s_connected = false;
        s_ip.addr = 0;
        bool creds = s_has_creds, scanning = s_scanning, ap_on = s_ap_on;
        UNLOCK();
        ESP_LOGW(TAG, "Disconnected (reason %d)", ev->reason);
        if (creds && !scanning) {
            schedule_reconnect();
        }
        if (was_connected && !ap_on) {
            ap_timer_arm(CONFIG_APP_AP_FALLBACK_DELAY_S);
        }
        break;
    }

    case WIFI_EVENT_AP_STACONNECTED:
        LOCK();
        s_ap_clients++;
        UNLOCK();
        break;

    case WIFI_EVENT_AP_STADISCONNECTED:
        LOCK();
        if (s_ap_clients) {
            s_ap_clients--;
        }
        UNLOCK();
        break;

    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "Connected to '%s', IP " IPSTR, s_ssid, IP2STR(&ev->ip_info.ip));
        LOCK();
        s_connected = true;
        s_ip = ev->ip_info.ip;
        s_backoff_ms = RECONNECT_MIN_MS;
        bool ap_on = s_ap_on;
        UNLOCK();
        esp_timer_stop(s_reconnect_timer);
        if (ap_on) {
            ap_timer_arm(AP_LINGER_S);
        } else {
            esp_timer_stop(s_ap_timer);
        }
    } else if (id == IP_EVENT_STA_LOST_IP) {
        LOCK();
        s_ip.addr = 0;
        UNLOCK();
    }
}

// ---------------------------------------------------------------------------
// Public API

static void apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t sta = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN,
            .pmf_cfg = { .capable = true, .required = false },
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    LOCK();
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    s_has_creds = ssid[0] != '\0';
    UNLOCK();
}

esp_err_t wifi_mgr_start(const char *hostname)
{
    s_lock = xSemaphoreCreateMutex();
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_sta, hostname));

    const esp_timer_create_args_t rt = { .callback = reconnect_timer_cb, .name = "wifi_rc" };
    const esp_timer_create_args_t at = { .callback = ap_timer_cb, .name = "wifi_ap" };
    ESP_ERROR_CHECK(esp_timer_create(&rt, &s_reconnect_timer));
    ESP_ERROR_CHECK(esp_timer_create(&at, &s_ap_timer));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // Credentials live in our own NVS namespace; don't let the driver keep a second copy.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%.24s-%02X%02X", CONFIG_APP_AP_SSID_PREFIX, mac[4], mac[5]);

    char ssid[SETTINGS_SSID_LEN], pass[SETTINGS_WIFI_PASS_LEN];
    bool have = settings_get_wifi(ssid, pass) == ESP_OK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (have) {
        apply_sta_config(ssid, pass);
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    if (have) {
        ESP_LOGI(TAG, "Connecting to '%s'", ssid);
        ap_timer_arm(CONFIG_APP_AP_FALLBACK_DELAY_S);
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi credentials");
        ap_enable(true);
    }
    return ESP_OK;
}

esp_err_t wifi_mgr_set_credentials(const char *ssid, const char *password)
{
    esp_err_t err = settings_set_wifi(ssid, password);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "New credentials for '%s'", ssid);
    esp_timer_stop(s_reconnect_timer);
    LOCK();
    s_backoff_ms = RECONNECT_MIN_MS;
    s_connected = false;
    UNLOCK();
    esp_wifi_disconnect();
    apply_sta_config(ssid, password);
    // Connect from the timer so the HTTP response goes out first.
    esp_timer_start_once(s_reconnect_timer, 500 * 1000);
    ap_timer_arm(CONFIG_APP_AP_FALLBACK_DELAY_S);
    return ESP_OK;
}

esp_err_t wifi_mgr_set_hostname(const char *hostname)
{
    esp_err_t err = esp_netif_set_hostname(s_sta, hostname);
    if (err != ESP_OK) {
        return err;
    }
    LOCK();
    bool connected = s_connected;
    UNLOCK();
    if (connected) {
        // Reconnect so the router learns the new name via DHCP.
        esp_wifi_disconnect();
    }
    return ESP_OK;
}

void wifi_mgr_get_status(wifi_mgr_status_t *st)
{
    memset(st, 0, sizeof(*st));
    LOCK();
    st->has_credentials = s_has_creds;
    st->sta_connected = s_connected;
    strlcpy(st->sta_ssid, s_ssid, sizeof(st->sta_ssid));
    if (s_ip.addr) {
        snprintf(st->sta_ip, sizeof(st->sta_ip), IPSTR, IP2STR(&s_ip));
    }
    st->ap_active = s_ap_on;
    st->ap_clients = s_ap_clients;
    strlcpy(st->ap_ssid, s_ap_ssid, sizeof(st->ap_ssid));
    UNLOCK();

    esp_read_mac(st->mac, ESP_MAC_WIFI_STA);
    if (st->sta_connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            st->rssi = ap.rssi;
        }
    }
    if (st->ap_active) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_ap, &info) == ESP_OK) {
            snprintf(st->ap_ip, sizeof(st->ap_ip), IPSTR, IP2STR(&info.ip));
        }
    }
}

esp_err_t wifi_mgr_scan(wifi_ap_record_t **records, uint16_t *count)
{
    *records = NULL;
    *count = 0;

    LOCK();
    if (s_scanning) {
        UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    s_scanning = true;
    bool connected = s_connected, creds = s_has_creds;
    UNLOCK();

    // A pending connection attempt makes the scan fail, so pause reconnecting.
    if (!connected) {
        esp_timer_stop(s_reconnect_timer);
        esp_wifi_disconnect();
    }
    const wifi_scan_config_t cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 100, .max = 300 },
    };
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n > 0) {
            *records = calloc(n, sizeof(wifi_ap_record_t));
            if (!*records) {
                esp_wifi_clear_ap_list();
                err = ESP_ERR_NO_MEM;
            } else {
                err = esp_wifi_scan_get_ap_records(&n, *records);
                *count = n;
            }
        }
    }

    LOCK();
    s_scanning = false;
    s_backoff_ms = RECONNECT_MIN_MS;
    UNLOCK();
    if (!connected && creds) {
        schedule_reconnect();
    }
    if (err != ESP_OK) {
        free(*records);
        *records = NULL;
        *count = 0;
    }
    return err;
}
