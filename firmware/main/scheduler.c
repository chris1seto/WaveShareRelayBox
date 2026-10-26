#include "scheduler.h"

#include <time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_services.h"
#include "relays.h"
#include "settings.h"

static const char *TAG = "sched";

static void run_minute(const struct tm *tm)
{
    schedule_t list[SCHEDULE_MAX];
    int n = settings_get_schedules(list);
    for (int i = 0; i < n; i++) {
        const schedule_t *e = &list[i];
        if (!e->enabled || e->hour != tm->tm_hour || e->minute != tm->tm_min ||
            !(e->days & (1u << tm->tm_wday))) {
            continue;
        }
        ESP_LOGI(TAG, "Schedule %d: %s relays 0x%02x", i + 1,
                 e->action == SCHED_ON ? "on" : e->action == SCHED_OFF ? "off" : "toggle", e->relays);
        if (e->action == SCHED_TOGGLE) {
            relays_set_mask(e->relays, ~relays_get_mask());
        } else {
            relays_set_mask(e->relays, e->action == SCHED_ON ? e->relays : 0);
        }
    }
}

static void scheduler_task(void *arg)
{
    int last = -1;   // minute of the week last handled
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!net_services_time_synced()) {
            continue;
        }
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        int minute = (tm.tm_wday * 24 + tm.tm_hour) * 60 + tm.tm_min;
        if (minute == last) {
            continue;
        }
        // The first minute after boot is only recorded: a reboot inside a scheduled
        // minute must not run a toggle twice.
        if (last >= 0) {
            run_minute(&tm);
        }
        last = minute;
    }
}

esp_err_t scheduler_start(void)
{
    return xTaskCreate(scheduler_task, "scheduler", 3072, NULL, 3, NULL) == pdPASS ? ESP_OK
                                                                                   : ESP_ERR_NO_MEM;
}
