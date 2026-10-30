// k210_link.h — the K210 link coordinator.
//
// Owns what the microphone, speaker, video and boot modules share:
//   - the half-duplex state (idle / listening / speaking),
//   - the RX task that parses UART frames and dispatches them,
//   - the ACK / ANIM_DONE / VERSION flags other tasks wait on,
//   - the power-enable and RESETN pins,
//   - the raw send API used by k210_controls.c.
//
// Protocol source of truth: FIRMWARE_HANDOFF.md. Frame dispatch (k210_link.c):
//   00 SPK_REQ -> speaker     01 MIC_READY -> microphone   08 ANIM_DONE -> flag
//   0B WAKE / END_OF_SPEECH -> microphone                  FE ACK -> flag
//   FF ext: 69 VERSION -> flag, anything else logged as text
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LINK_IDLE = 0, LINK_LISTEN = 1, LINK_SPEAK = 2 } link_state_t;

// Power-enable HIGH, RESETN high, UART + SPI up, RX task running. The boot
// sequence is started separately (k210_boot_start). microphone_init() and
// speaker_init() must run before this.
esp_err_t k210_link_start(void);

// Half-duplex state. Written by microphone.c / speaker.c / k210_boot.c only.
link_state_t k210_link_state(void);
void         k210_link_set_state(link_state_t st);

// True while the boot sequence owns the link (raw sends and mic/speaker wait).
bool k210_link_booting(void);
void k210_link_set_booting(bool on);

// Flags set by the RX task; the wait functions poll every 5 ms from any other
// task (never from the RX task itself).
void k210_link_clear_ack(void);
bool k210_link_wait_ack(uint32_t timeout_ms);
void k210_link_clear_anim_done(void);
void k210_link_set_anim_done(void);           // when no animation was requested
bool k210_link_wait_anim_done(uint32_t timeout_ms);
void k210_link_clear_version(void);
bool k210_link_version_seen(void);
const char *k210_link_version(void);          // "" until the K210 answered

// RESETN pulse (active low), then 50 ms settle.
void k210_link_reset_pulse(uint32_t low_ms);

// Drop the power-enable pin: the whole robot (K210, ESP32, servos) switches
// off, exactly like holding the button. Returns only while on the charger,
// where the board may stay powered.
void k210_link_power_off(void);

// Raw frames for /api/controls. Fail with ESP_ERR_INVALID_STATE while booting.
esp_err_t k210_send_short(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3);
esp_err_t k210_send_ext(uint8_t sub, const char *payload, bool *acked);   // waits <= 500 ms for ACK

typedef struct {
    uint32_t rx_frames;     // UART frames parsed from the K210
    uint32_t bad_cksum;     // UART frames rejected on checksum
    uint32_t spi_reads;     // mic SPI transfers done
    uint32_t spi_writes;    // speaker SPI transfers done
    uint32_t mic_samples;   // total mic samples delivered
    uint32_t play_queued;   // 48 kHz samples waiting in the speaker buffer
    uint32_t play_requests; // SPK_REQ frames seen
    uint32_t wake_events;   // WAKE frames (K210 heard its wake word)
    uint16_t mic_peak;      // |peak| of the last mic chunk, 0..32767
    uint8_t  state;         // 0 idle, 1 listening (mic), 2 speaking
    char     version[16];   // K210 firmware version string, "" until seen
    bool     linked;        // a valid frame seen within the last 15 s
    bool     boot_done;     // boot sequence completed
    bool     mic_open;      // MODE_MIC sent and no END_OF_SPEECH yet
} k210_stats_t;
void k210_get_stats(k210_stats_t *out);

#ifdef __cplusplus
}
#endif
