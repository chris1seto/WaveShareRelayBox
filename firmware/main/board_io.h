#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * Status LED (WS2812) and passive buzzer on the Waveshare board.
 *
 * LED colours: green = Wi-Fi and MQTT fine (or MQTT off), amber = Wi-Fi up but MQTT
 * not connected, blinking blue = setup access point, blinking red = no network.
 * It flashes white when a relay changes. The buzzer chirps on relay changes when
 * enabled in settings.
 */
esp_err_t board_io_start(void);

/** Re-read LED/buzzer settings after they change. */
void board_io_reload(void);

/** Sound the buzzer for `ms` milliseconds at `freq_hz`. Returns immediately. */
void board_io_beep(uint32_t ms, uint32_t freq_hz);

/** Show the factory-reset countdown (fast red blink) while the button is held. */
void board_io_reset_pending(bool pending);
