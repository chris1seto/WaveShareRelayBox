#include "board_io.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "mqtt_link.h"
#include "relays.h"
#include "settings.h"
#include "wifi_mgr.h"

static const char *TAG = "board_io";

#define BUZZER_MODE    LEDC_LOW_SPEED_MODE
#define BUZZER_TIMER   LEDC_TIMER_0
#define BUZZER_CHANNEL LEDC_CHANNEL_0

static led_strip_handle_t s_led;
static esp_timer_handle_t s_beep_timer;
static bool s_buzzer_ok;
static volatile hw_settings_t s_hw;
static volatile int64_t s_flash_until_us;
static volatile bool s_reset_pending;

// ---------------------------------------------------------------------------
// Buzzer (passive: needs a square wave, so it runs off an LEDC PWM channel)

static void beep_stop(void *arg)
{
    ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 0);
    ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
}

void board_io_beep(uint32_t ms, uint32_t freq_hz)
{
    if (!s_buzzer_ok) {
        return;
    }
    ledc_set_freq(BUZZER_MODE, BUZZER_TIMER, freq_hz);
    ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 512);   // 50% of 10-bit
    ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
    esp_timer_stop(s_beep_timer);
    esp_timer_start_once(s_beep_timer, (uint64_t)ms * 1000);
}

static void buzzer_init(void)
{
#if CONFIG_BOARD_BUZZER_GPIO >= 0
    const ledc_timer_config_t t = {
        .speed_mode = BUZZER_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BUZZER_TIMER,
        .freq_hz = 2700,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t c = {
        .gpio_num = CONFIG_BOARD_BUZZER_GPIO,
        .speed_mode = BUZZER_MODE,
        .channel = BUZZER_CHANNEL,
        .timer_sel = BUZZER_TIMER,
        .duty = 0,
    };
    const esp_timer_create_args_t ta = { .callback = beep_stop, .name = "beep" };
    if (ledc_timer_config(&t) == ESP_OK && ledc_channel_config(&c) == ESP_OK &&
        esp_timer_create(&ta, &s_beep_timer) == ESP_OK) {
        s_buzzer_ok = true;
    } else {
        ESP_LOGW(TAG, "Buzzer init failed");
    }
#endif
}

// ---------------------------------------------------------------------------
// Status LED

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led) {
        return;
    }
    uint32_t k = s_hw.status_led ? s_hw.led_brightness : 0;   // percent
    led_strip_set_pixel(s_led, 0, r * k / 100, g * k / 100, b * k / 100);
    led_strip_refresh(s_led);
}

static void led_task(void *arg)
{
    uint32_t tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        tick++;
        bool blink = (tick / 5) % 2;   // 1 Hz
        if (s_reset_pending) {
            led_set((tick % 2) ? 255 : 0, 0, 0);
            continue;
        }
        if (esp_timer_get_time() < s_flash_until_us) {
            led_set(255, 255, 255);
            continue;
        }
        wifi_mgr_status_t ws;
        wifi_mgr_get_status(&ws);
        if (ws.sta_connected) {
            if (mqtt_link_state() == MQTT_LINK_CONNECTING) {
                led_set(255, 120, 0);   // amber
            } else {
                led_set(0, 255, 0);
            }
        } else if (ws.ap_active) {
            led_set(0, 0, blink ? 255 : 0);
        } else {
            led_set(blink ? 255 : 0, 0, 0);
        }
    }
}

static void led_init(void)
{
#if CONFIG_BOARD_LED_GPIO >= 0
    const led_strip_config_t cfg = {
        .strip_gpio_num = CONFIG_BOARD_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt = { .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&cfg, &rmt, &s_led) != ESP_OK) {
        ESP_LOGW(TAG, "Status LED init failed");
        s_led = NULL;
        return;
    }
    led_strip_clear(s_led);
#endif
}

// ---------------------------------------------------------------------------

static void on_relays_changed(uint32_t old_mask, uint32_t new_mask)
{
    s_flash_until_us = esp_timer_get_time() + 150 * 1000;
    if (s_hw.buzzer) {
        // Higher pitch when something switched on, lower when only switching off.
        board_io_beep(60, (new_mask & ~old_mask) ? 3200 : 2200);
    }
}

void board_io_reload(void)
{
    device_settings_t ds;
    settings_get(&ds);
    s_hw = ds.hw;
}

void board_io_reset_pending(bool pending)
{
    s_reset_pending = pending;
}

esp_err_t board_io_start(void)
{
    board_io_reload();
    buzzer_init();
    led_init();
    relays_add_listener(on_relays_changed);
    if (s_led && xTaskCreate(led_task, "status_led", 3072, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
