// k210_spi.h — SPI transport for the K210 audio data (FIRMWARE_HANDOFF.md §5).
//
// ESP32 master, 8 MHz, mode 1, MSB first, CS = GPIO13 driven by hand so the
// command and the data get separate CS windows:
//   command phase  8 bytes [0 0 lenL lenH mode chk 0 0], chk = lenL+lenH+mode
//   gap            >= 0.5 ms (read) / >= 1 ms (write)
//   data phase     len rounded up to a multiple of 64 bytes
// Every 32-bit word on the wire is byte-reversed (k210_swap4): the mic read
// swaps after receiving, the speaker write swaps before sending.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define K210_SPI_MAX_XFER 16384             // largest data phase (speaker block + margin)

// Bus + device + CS pin. Allocates nothing per transfer.
esp_err_t k210_spi_init(void);

// Write `len` bytes from `wire` (needs 64 bytes of slack after len, DMA-capable
// RAM, already in wire byte order) with command mode byte `mode`.
void k210_spi_write(uint8_t *wire, uint16_t len, uint8_t mode, uint32_t gap_us);

// Read `len` bytes into `dst` (same slack / RAM requirements) with mode `mode`.
void k210_spi_read(uint8_t *dst, uint16_t len, uint8_t mode, uint32_t gap_us);

// b0 b1 b2 b3 -> b3 b2 b1 b0 for every 4-byte group of `len` bytes (rounded up).
void k210_swap4(uint8_t *p, size_t len);

// Transfer counters for the status page.
void k210_spi_counts(uint32_t *reads, uint32_t *writes);

#ifdef __cplusplus
}
#endif
