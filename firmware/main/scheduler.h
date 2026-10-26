#pragma once

#include "esp_err.h"

/**
 * Run the schedules stored in settings. Each enabled entry switches its relays at
 * hour:minute local time on the selected weekdays. Nothing runs until NTP has set the
 * clock, and a minute missed while the device was off or the clock jumped is skipped,
 * not caught up.
 */
esp_err_t scheduler_start(void);
