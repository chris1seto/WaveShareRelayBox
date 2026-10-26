#include "net_services.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "mdns.h"

static const char *TAG = "netsvc";

// lwIP keeps a pointer to the server name, so it must outlive the SNTP session.
static char s_ntp_server[64];
static volatile bool s_synced;

static void on_time_sync(struct timeval *tv)
{
    s_synced = true;
    char buf[32];
    net_services_local_time(buf, sizeof(buf));
    ESP_LOGI(TAG, "Time synchronised: %s", buf);
}

static void apply_timezone(const char *tz)
{
    setenv("TZ", (tz && tz[0]) ? tz : "UTC0", 1);
    tzset();
}

static esp_err_t sntp_restart(const char *server)
{
    esp_netif_sntp_deinit();
    snprintf(s_ntp_server, sizeof(s_ntp_server), "%s", server);
    if (!s_ntp_server[0]) {
        ESP_LOGI(TAG, "NTP disabled");
        return ESP_OK;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_ntp_server);
    cfg.sync_cb = on_time_sync;
    cfg.wait_for_sync = false;
    ESP_LOGI(TAG, "NTP server: %s", s_ntp_server);
    return esp_netif_sntp_init(&cfg);
}

static esp_err_t mdns_apply_hostname(const char *hostname)
{
    char instance[64];
    snprintf(instance, sizeof(instance), "Waveshare Relay (%s)", hostname);
    esp_err_t err = mdns_hostname_set(hostname);
    if (err == ESP_OK) {
        err = mdns_instance_name_set(instance);
    }
    return err;
}

esp_err_t net_services_start(const char *hostname, const char *ntp_server, const char *timezone)
{
    apply_timezone(timezone);

    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        mdns_apply_hostname(hostname);
        const esp_app_desc_t *app = esp_app_get_description();
        mdns_txt_item_t txt[] = {
            { "board", "ESP32-S3-Relay-6CH" },
            { "version", app->version },
            { "path", "/" },
        };
        mdns_service_add(NULL, "_http", "_tcp", 80, txt, sizeof(txt) / sizeof(txt[0]));
        ESP_LOGI(TAG, "mDNS: http://%s.local/", hostname);
    } else {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
    }

    return sntp_restart(ntp_server);
}

esp_err_t net_services_set_hostname(const char *hostname)
{
    return mdns_apply_hostname(hostname);
}

esp_err_t net_services_set_time_config(const char *ntp_server, const char *timezone)
{
    apply_timezone(timezone);
    if (strcmp(ntp_server, s_ntp_server) != 0) {
        return sntp_restart(ntp_server);
    }
    return ESP_OK;
}

bool net_services_time_synced(void)
{
    return s_synced;
}

void net_services_local_time(char *buf, size_t len)
{
    time_t now = time(NULL);
    // Anything before 2024 means the clock was never set.
    if (now < 1704067200) {
        buf[0] = '\0';
        return;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
}
