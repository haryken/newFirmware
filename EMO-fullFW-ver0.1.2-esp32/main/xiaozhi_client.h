// xiaozhi_client.h — Xiaozhi OTA + WebSocket voice client over the EMO K210 path.
// Mic PCM (16 kHz) -> Opus -> server; TTS Opus -> PCM 48 kHz -> speaker.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void xiaozhi_init(void);

// Background: wait for STA IP, OTA fetch (activation / websocket.url), ready for wake.
void xiaozhi_start(void);

bool xiaozhi_enabled(void);          // settings voice_mode == xiaozhi
bool xiaozhi_ready(void);            // have a websocket URL (OTA or override)
bool xiaozhi_connected(void);        // WS open + hello done
bool xiaozhi_in_session(void);

void xiaozhi_session_begin(void);    // wake word / barge-in
void xiaozhi_session_end(void);      // end-of-speech -> listen stop
void xiaozhi_feed(const int16_t *pcm, size_t samples);

typedef struct {
    bool     enabled;
    bool     ready;
    bool     connected;
    bool     session;
    bool     speaking;
    bool     listening;
    char     activation[16];         // 6-digit code, or ""
    char     device_id[18];          // MAC aa:bb:cc:dd:ee:ff
    char     client_id[40];          // UUID persisted in NVS
    char     ws_url[160];
    uint32_t sessions;
    uint32_t opus_sent;
    uint32_t opus_recv;
} xiaozhi_stats_t;

void xiaozhi_get_stats(xiaozhi_stats_t *out);

// Force one OTA POST now (needs STA + internet). Fills activation / websocket URL.
// Returns ESP_OK when the HTTP round-trip succeeded (code may still be empty if already paired).
esp_err_t xiaozhi_pair_refresh(void);

// Hide the pair-code text on the face (after linking on xiaozhi.me).
void xiaozhi_dismiss_activation(void);

#ifdef __cplusplus
}
#endif
