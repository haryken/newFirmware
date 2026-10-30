// host_stream.h — mic -> voice server streaming, one TCP connection per utterance.
//
// Host address is fixed at build time (menuconfig: EMO_HOST_IP / EMO_HOST_PORT).
//
// Wire format (robot -> host, over TCP):
//   Hello (once per connection):
//     "EMOA" ver=1  sr(4 LE)=16000  ch(1)=1  bits(1)=16  start_off(8 LE)
//   Chunk (repeated):
//     off(8 LE)  len(2 LE)  crc32(4 LE)  payload[len]   (int16 LE mono 16 kHz)
//   Host -> robot:
//     "ACK0" off(8 LE)         cumulative bytes safely received
//
// Lifecycle: host_stream_session_begin() (on the K210's wake word) connects to
// the host and streams every mic sample fed from then on; samples are kept in a
// PSRAM backlog so a slow connect or a Wi-Fi drop loses nothing. On
// host_stream_session_end() (end of the utterance) the remaining bytes are
// flushed, the socket is closed, and the backlog is discarded. Mic audio fed
// outside a session only reaches the monitor tap (web page meter / WAV capture).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Monitor tap: receives all mic PCM, in or out of a session.
typedef void (*host_stream_monitor_cb_t)(const int16_t *pcm, size_t samples, void *ctx);

void host_stream_init(void);

// Utterance boundaries, driven by the K210 wake word / end of speech.
void host_stream_session_begin(void);
void host_stream_session_end(void);
bool host_stream_in_session(void);

// Feed mic PCM (16 kHz mono int16 LE). Thread-safe.
void host_stream_feed(const int16_t *pcm, size_t samples);

// Register (or clear with NULL) the monitor tap.
void host_stream_set_monitor(host_stream_monitor_cb_t cb, void *ctx);

typedef struct {
    bool     connected;      // TCP session to host is up
    bool     session;        // between wake word and end of speech
    uint32_t sessions;       // utterances started since boot
    uint64_t fed;            // total mic bytes fed into the backlog
    uint64_t acked;          // bytes host confirmed
    uint32_t backlog;        // bytes currently buffered (unacked)
    uint32_t overflow;       // bytes dropped because backlog was full
    uint32_t reconnects;     // TCP connections made
    int16_t  peak;           // last-window peak sample (for the UI meter)
    float    rms;            // last-window RMS (0..32767)
} host_stream_stats_t;
void host_stream_get_stats(host_stream_stats_t *out);

#ifdef __cplusplus
}
#endif
