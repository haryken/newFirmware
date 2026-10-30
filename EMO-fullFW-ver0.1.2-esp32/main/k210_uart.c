// k210_uart.c — see k210_uart.h.
#include "k210_uart.h"
#include "sdkconfig.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

static const char *TAG = "k210_uart";

#define UART_PORT   CONFIG_EMO_K210_UART_NUM
#define UART_BAUD   1000000
#define UART_TX     CONFIG_EMO_K210_UART_TX
#define UART_RX     CONFIG_EMO_K210_UART_RX

esp_err_t k210_uart_init(void)
{
    uart_config_t uc = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t e;
    if ((e = uart_driver_install(UART_PORT, 16384, 1024, 0, NULL, 0)) != ESP_OK) return e;
    if ((e = uart_param_config(UART_PORT, &uc)) != ESP_OK) return e;
    if ((e = uart_set_pin(UART_PORT, UART_TX, UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE)) != ESP_OK) return e;
    uart_flush_input(UART_PORT);
    ESP_LOGI(TAG, "UART%d %d baud, TX GPIO%d, RX GPIO%d", UART_PORT, UART_BAUD, UART_TX, UART_RX);
    return ESP_OK;
}

static void uart_send(const uint8_t *p, size_t n)
{
    uart_write_bytes(UART_PORT, p, n);
    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(200));
}

void k210_uart_send_short(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3)
{
    uint8_t f[7];
    f[0] = 0x55; f[1] = 0xAA;
    f[2] = cmd; f[3] = p1; f[4] = p2; f[5] = p3;
    f[6] = (uint8_t)(cmd + p1 + p2 + p3 - 1);
    if (K210_LINK_DEBUG >= 2)
        ESP_LOGI(TAG, "TX 55 AA %02X %02X %02X %02X %02X", cmd, p1, p2, p3, f[6]);
    uart_send(f, sizeof(f));
}

void k210_uart_send_mode(uint8_t pp) { k210_uart_send_short(0x06, pp, 0x10, 0x00); }

uint8_t k210_crc8(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) c = (c & 1) ? (uint8_t)((c >> 1) ^ 0x8C) : (uint8_t)(c >> 1);
    }
    return c;
}

bool k210_uart_send_ext(uint8_t sub, const char *str)
{
    size_t len = strlen(str) + 1;            // includes the NUL
    if (len > 255) return false;
    uint8_t f[8 + 255];
    f[0] = 0x55; f[1] = 0xAA; f[2] = 0xFF;
    f[3] = len & 0xFF; f[4] = len >> 8; f[5] = sub;
    f[6] = (uint8_t)(0xFF + f[3] + f[4] + sub - 1);
    memcpy(f + 7, str, len);
    f[7 + len] = k210_crc8(f + 7, len);
    if (K210_LINK_DEBUG >= 2)
        ESP_LOGI(TAG, "TX ext sub %02X len %u crc %02X: %s", sub, (unsigned)len, f[7 + len], str);
    uart_send(f, 8 + len);
    return true;
}

int k210_uart_read(uint8_t *dst, size_t n, uint32_t timeout_ms)
{
    int m = uart_read_bytes(UART_PORT, dst, n, pdMS_TO_TICKS(timeout_ms));
    return m < 0 ? 0 : m;
}
