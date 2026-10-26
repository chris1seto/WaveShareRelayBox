#include "modbus.h"

#include <inttypes.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "relays.h"
#include "settings.h"

static const char *TAG = "modbus";

#define MB_UART      UART_NUM_1
#define MB_BUF       256
#define MB_COILS     RELAY_COUNT

// Exception codes
#define EX_ILLEGAL_FUNCTION  0x01
#define EX_ILLEGAL_ADDRESS   0x02
#define EX_ILLEGAL_VALUE     0x03

static volatile bool s_reload = true;
static modbus_stats_t s_stats;

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return crc;
}

static void send_frame(uint8_t *f, size_t n)
{
    uint16_t crc = crc16(f, n);
    f[n] = crc & 0xFF;
    f[n + 1] = crc >> 8;
    uart_write_bytes(MB_UART, f, n + 2);
}

static void send_exception(uint8_t addr, uint8_t fc, uint8_t code)
{
    uint8_t f[5] = { addr, (uint8_t)(fc | 0x80), code };
    s_stats.exceptions++;
    send_frame(f, 3);
}

/** Handle one request. Returns the reply length written to `out` (0 = no reply). */
static size_t handle(const uint8_t *req, size_t n, uint8_t *out, uint8_t *exception)
{
    uint8_t fc = req[1];
    uint16_t a = (req[2] << 8) | req[3];
    uint16_t b = (req[4] << 8) | req[5];
    *exception = 0;
    memcpy(out, req, 2);

    switch (fc) {
    case 0x01: {   // read coils: a = start, b = count
        if (b < 1 || b > MB_COILS) { *exception = EX_ILLEGAL_VALUE; return 0; }
        if (a + b > MB_COILS) { *exception = EX_ILLEGAL_ADDRESS; return 0; }
        out[2] = 1;
        out[3] = (uint8_t)((relays_get_mask() >> a) & ((1u << b) - 1));
        return 4;
    }
    case 0x03: {   // read holding registers: only register 0
        if (b != 1) { *exception = EX_ILLEGAL_VALUE; return 0; }
        if (a != 0) { *exception = EX_ILLEGAL_ADDRESS; return 0; }
        uint32_t m = relays_get_mask();
        out[2] = 2;
        out[3] = 0;
        out[4] = (uint8_t)m;
        return 5;
    }
    case 0x05: {   // write single coil: b = 0xFF00 on, 0x0000 off
        if (a >= MB_COILS) { *exception = EX_ILLEGAL_ADDRESS; return 0; }
        if (b != 0xFF00 && b != 0x0000) { *exception = EX_ILLEGAL_VALUE; return 0; }
        relays_set(a, b == 0xFF00);
        memcpy(out, req, 6);   // echo
        return 6;
    }
    case 0x06: {   // write single register 0 = relay bitmask
        if (a != 0) { *exception = EX_ILLEGAL_ADDRESS; return 0; }
        if (b >> RELAY_COUNT) { *exception = EX_ILLEGAL_VALUE; return 0; }
        relays_set_mask((1u << RELAY_COUNT) - 1, b);
        memcpy(out, req, 6);
        return 6;
    }
    case 0x0F: {   // write multiple coils: a = start, b = count, req[6] = byte count
        if (n < 8 || b < 1 || b > MB_COILS || req[6] != (b + 7) / 8 || n < 7u + req[6]) {
            *exception = EX_ILLEGAL_VALUE;
            return 0;
        }
        if (a + b > MB_COILS) { *exception = EX_ILLEGAL_ADDRESS; return 0; }
        uint32_t which = ((1u << b) - 1) << a;
        relays_set_mask(which, (uint32_t)req[7] << a);
        memcpy(out, req, 6);
        return 6;
    }
    default:
        *exception = EX_ILLEGAL_FUNCTION;
        return 0;
    }
}

static bool configure(uint8_t *addr)
{
    device_settings_t ds;
    settings_get(&ds);
    if (uart_is_driver_installed(MB_UART)) {
        uart_driver_delete(MB_UART);
    }
    if (!ds.hw.modbus_enabled) {
        ESP_LOGI(TAG, "Disabled");
        return false;
    }
    const uart_config_t cfg = {
        .baud_rate = (int)ds.hw.modbus_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(MB_UART, MB_BUF * 2, MB_BUF * 2, 0, NULL, 0) != ESP_OK ||
        uart_param_config(MB_UART, &cfg) != ESP_OK ||
        uart_set_pin(MB_UART, CONFIG_BOARD_RS485_TX_GPIO, CONFIG_BOARD_RS485_RX_GPIO,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "UART setup failed");
        uart_driver_delete(MB_UART);
        return false;
    }
    *addr = ds.hw.modbus_address;
    ESP_LOGI(TAG, "RTU slave, address %u, %" PRIu32 " baud 8N1", *addr, ds.hw.modbus_baud);
    return true;
}

static void modbus_task(void *arg)
{
    uint8_t req[MB_BUF], out[MB_BUF];
    uint8_t addr = 1;
    bool running = false;

    for (;;) {
        if (s_reload) {
            s_reload = false;
            running = configure(&addr);
        }
        if (!running) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        // A frame ends at a silent gap. One tick (10 ms) is longer than 3.5 characters
        // at 9600 baud and above, and masters wait for our reply before sending again.
        int n = uart_read_bytes(MB_UART, req, sizeof(req), pdMS_TO_TICKS(500));
        if (n <= 0) {
            continue;
        }
        int more;
        while (n < (int)sizeof(req) &&
               (more = uart_read_bytes(MB_UART, req + n, sizeof(req) - n, pdMS_TO_TICKS(10))) > 0) {
            n += more;
        }
        if (n < 8) {
            continue;   // shortest request we handle is 8 bytes
        }
        if (crc16(req, n - 2) != (req[n - 2] | (req[n - 1] << 8))) {
            s_stats.crc_errors++;
            continue;
        }
        if (req[0] != addr && req[0] != 0) {
            continue;   // for another slave
        }
        uint8_t ex;
        size_t len = handle(req, n - 2, out, &ex);
        if (req[0] == 0) {
            s_stats.frames_ok++;
            continue;   // broadcast: never reply
        }
        if (ex) {
            send_exception(addr, req[1], ex);
        } else {
            s_stats.frames_ok++;
            send_frame(out, len);
        }
    }
}

esp_err_t modbus_start(void)
{
    return xTaskCreate(modbus_task, "modbus", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK
                                                                             : ESP_ERR_NO_MEM;
}

void modbus_reload(void)
{
    s_reload = true;
}

void modbus_get_stats(modbus_stats_t *out)
{
    *out = s_stats;
}
