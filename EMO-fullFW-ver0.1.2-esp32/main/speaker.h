// speaker.h — the robot speaker (K210 MODE_SPK).
//
// Protocol (FIRMWARE_HANDOFF.md): in MODE_SPK the K210 sends SPK_REQ
// (55 AA 00 00 30 00 2F) every 64.5 ms; we answer over SPI (mode byte 0) with a
// 12288-byte block = 3072 frames of 48 kHz stereo (L=R) big-endian int16.
// Unserved requests are fine. After the last block ~13 requests are ignored so
// the K210 drains, then MODE_IDLE re-arms the wake word. The K210 is
// half-duplex: entering MODE_SPK closes the mic.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Allocates the playback ring (PSRAM). Before k210_link_start().
esp_err_t speaker_init(void);

// Queue PCM: signed 16-bit LE MONO at 48 kHz. Returns samples accepted
// (< len when the ~0.68 s ring is full; the caller throttles).
size_t speaker_play(const int16_t *pcm, size_t samples);
void   speaker_flush(void);

// Queue a sine test tone (blocks until queued).
void speaker_tone(unsigned hz, unsigned ms, int16_t amplitude);

// Barge-in: drop everything queued and leave MODE_SPK now (the wake word was
// heard while speaking, or the K210 stopped asking for blocks). Bumps the
// serial so a feeder in /api/play_pcm knows its clip was cut.
void     speaker_interrupt(const char *why);
uint32_t speaker_serial(void);
bool     speaker_ready(void);                 // ring allocated
bool     speaker_busy(void);                  // MODE_SPK or audio still queued
void     speaker_end_handoff(void);           // stop silence-feed after SPEAK→MIC

// Fired (from link RX) when playback drains — keep the callback light.
// Return true to take over the link (e.g. go MODE_MIC without IDLE).
typedef bool (*speaker_idle_cb_t)(void);
void speaker_set_idle_cb(speaker_idle_cb_t cb);

// Link RX hooks (k210_link.c only).
void speaker_on_spk_req(uint16_t len);        // 00: serve one block
void speaker_tick(int64_t now_us);            // enter MODE_SPK when audio is queued; SPK_REQ watchdog

typedef struct {
    uint32_t queued_samples;    // 48 kHz mono samples waiting
    uint32_t requests;          // SPK_REQ frames seen
    uint32_t interrupts;        // clips cut short (barge-in / K210 stopped requesting)
} speaker_stats_t;
void speaker_get_stats(speaker_stats_t *out);

#ifdef __cplusplus
}
#endif
