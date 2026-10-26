#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/** Start mDNS (<hostname>.local + _http._tcp) and SNTP, and apply the timezone. */
esp_err_t net_services_start(const char *hostname, const char *ntp_server, const char *timezone);

esp_err_t net_services_set_hostname(const char *hostname);

/** Restart SNTP with a new server and/or apply a new POSIX timezone. */
esp_err_t net_services_set_time_config(const char *ntp_server, const char *timezone);

bool net_services_time_synced(void);

/** Local time as "YYYY-MM-DD HH:MM:SS", or "" if the clock isn't set yet. */
void net_services_local_time(char *buf, size_t len);
