// k210_file.h - read a file from the K210 storage (ext 0x03, data comes back over SPI).
// Protocol from the stock firmware (uart_commands.md 1.3): 0x2C begin session, ext 0x03 <path>,
// then RX 0x04 (len = d0 + d1*256) chunks -> SPI read type 0x02, RX 0x05 = done, 0x2D end.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// RX hooks, called from k210_link.c
void k210_file_on_chunk(uint16_t n);
void k210_file_on_done(void);

// Reads up to `cap` bytes of `path` into `dst`. *len = bytes received.
esp_err_t k210_file_read(const char *path, uint8_t *dst, size_t cap, size_t *len, int timeout_ms);

#ifdef __cplusplus
}
#endif
