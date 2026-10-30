// k210_uart.h — UART1 transport to the K210 and the two frame encodings.
//
//   Short frame     55 AA cmd p1 p2 p3 chk          chk = (cmd+p1+p2+p3-1) & 0xFF
//   Extended frame  55 AA FF lenL lenH sub chk  <payload incl. NUL>  crc8
//                   chk = (FF+lenL+lenH+sub-1) & 0xFF, crc8 = CRC-8/MAXIM of the payload
//   1 Mbaud 8N1, TX GPIO12 -> K210, RX GPIO4 <- K210 (FIRMWARE_HANDOFF.md).
//
// Only the encoding and the wire live here. What a frame means, and who waits
// for the answer, is k210_link.c and the modules above it.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Console tracing of the link: 0 quiet, 1 every frame except the audio
// handshakes (MIC_READY, SPK_REQ, mic level), 2 everything incl. our TX.
#define K210_LINK_DEBUG 1

esp_err_t k210_uart_init(void);

// 55 AA cmd p1 p2 p3 chk. Whole frames are written atomically (driver mutex),
// so any task may send.
void k210_uart_send_short(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3);

// 06 pp 10 00: the audio mode word (K210_MODE_IDLE / MIC / SPK).
#define K210_MODE_IDLE 0x01
#define K210_MODE_MIC  0x03
#define K210_MODE_SPK  0x09
void k210_uart_send_mode(uint8_t pp);

// Extended frame with a NUL-terminated string payload (<= 254 chars).
// Returns false (nothing sent) when the payload is too long.
bool k210_uart_send_ext(uint8_t sub, const char *str);

// CRC-8/MAXIM (reflected poly 0x8C, init 0, no final xor).
uint8_t k210_crc8(const uint8_t *p, size_t n);

// Blocking reads for the RX task. Return the number of bytes read (0 = timeout).
int k210_uart_read(uint8_t *dst, size_t n, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
