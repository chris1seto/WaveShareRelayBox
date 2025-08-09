#ifndef RELAY_CONTROL_H
#define RELAY_CONTROL_H

#include "esp_err.h"
#include "driver/gpio.h"

// Number of relays on the Waveshare ESP32-S3-Relay-6CH
#define NUM_RELAYS 6

// GPIO pins for relays (adjust according to your board)
#define RELAY_1_GPIO GPIO_NUM_2
#define RELAY_2_GPIO GPIO_NUM_3
#define RELAY_3_GPIO GPIO_NUM_41
#define RELAY_4_GPIO GPIO_NUM_42
#define RELAY_5_GPIO GPIO_NUM_45
#define RELAY_6_GPIO GPIO_NUM_46

// Relay states
#define RELAY_ON  1
#define RELAY_OFF 0

// Function declarations
esp_err_t relay_control_init(void);
esp_err_t relay_set_state(int relay_id, bool state);
bool relay_get_state(int relay_id);
esp_err_t relay_set_multiple(const char* json_data);
void relay_publish_status(void);

// External variables
extern int relay_states[NUM_RELAYS];
extern const int relay_gpios[NUM_RELAYS];

#endif // RELAY_CONTROL_H
