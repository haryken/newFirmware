// microphone.h — the robot microphone (K210 MODE_MIC) and the utterance session.
//
// Protocol (FIRMWARE_HANDOFF.md): after MODE_MIC the K210 sends MIC_READY
// (55 AA 01 00 04 00 04) every 32.2 ms; we read 1024 bytes over SPI with mode
// byte 1: each 4-byte word holds two 16 kHz mono samples, big-endian, later
// sample first (== swap4 then LE16). The K210 runs its own VAD and ends a window
// with 0B 00 (END_OF_SPEECH); 0B 01 xx is the wake word.
//
// Utterance window: the K210's VAD cuts ~0.7 s after any pause, which splits
// sentences at a breath. After a wake word we treat END_OF_SPEECH as a hint: if
// voice was heard within HANGOVER we re-open a follow-up window (4B + MODE_MIC)
// and end the turn ourselves after END_SILENCE of quiet, at SESSION_MAX, or
// when the K210 stops sending frames (MIC_STALL, e.g. it choked on its SD card).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Mic PCM: signed 16-bit LE mono at 16 kHz, 512 samples per call, from the
// link RX task. Copy out quickly.
typedef void (*microphone_cb_t)(const int16_t *pcm, size_t samples);
void microphone_init(microphone_cb_t cb);

// Utterance events, delivered from the link RX task (keep handlers short).
typedef enum { MIC_EV_WAKE, MIC_EV_END_OF_SPEECH } microphone_event_t;
typedef void (*microphone_event_cb_t)(microphone_event_t ev);
void microphone_set_event_cb(microphone_event_cb_t cb);

// Open a listen window now (4B + MODE_MIC), outside a wake-word session; used
// by the web WAV capture and the "mode_mic" control. Fails while booting or speaking.
esp_err_t microphone_open(void);

// Called by speaker.c when it takes the link: the mic is closed by MODE_SPK.
void microphone_mark_closed(void);

// Link RX hooks (k210_link.c only).
void microphone_on_mic_ready(uint16_t len);               // 01: read + deliver a frame
void microphone_on_voice_event(uint8_t p1, uint8_t p2);   // 0B: WAKE / END_OF_SPEECH
void microphone_tick_reopen(int64_t now_us);              // pending re-open after WAKE / a cut
void microphone_tick_watchdog(int64_t now_us);            // stalled / overlong session

typedef struct {
    uint32_t samples;       // total mic samples delivered
    uint32_t wake_events;   // WAKE frames seen
    uint16_t peak;          // |peak| of the last frame
    bool     open;          // MODE_MIC sent and no END_OF_SPEECH yet
    bool     in_session;    // wake word heard, utterance in progress
} microphone_stats_t;
void microphone_get_stats(microphone_stats_t *out);

#ifdef __cplusplus
}
#endif
