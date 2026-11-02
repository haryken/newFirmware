// speaker.c — see speaker.h.
#include "speaker.h"
#include "k210_link.h"
#include "k210_uart.h"
#include "k210_spi.h"
#include "microphone.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "spk";

#define SPK_BLOCK         12288           // 3072 frames @ 48 kHz stereo per SPK_REQ
#define PLAY_RING_BYTES   (1024 * 1024)   // ~5.4 s of speaker audio, in PSRAM
#define PLAY_RING_SMALL   (128 * 1024)    // fallback without PSRAM (~0.7 s)
#define PLAY_REQ_MAX      K210_SPI_MAX_XFER
#define SPK_DRAIN_REQS    20              // ignored SPK_REQs before MODE_IDLE (~1.3 s)
#define SPK_START_BYTES   (2 * SPK_BLOCK) // buffer this much before MODE_SPK (>=128 ms)
#define SPK_LIVE_US       1200000         // audio queued this recently = stream still live
#define SPK_STALL_US      200000          // no new audio for this long = end of stream
#define SPK_REQ_TIMEOUT_US 1500000        // in MODE_SPK but no SPK_REQ for this long: the K210 left

static RingbufHandle_t s_ring;            // byte ring of wire-ready blocks
static volatile int64_t s_last_enqueue_us;
static volatile int64_t s_last_req_us;
static int s_ignored;
static uint32_t s_requests, s_interrupts;
static volatile uint32_t s_serial;        // bumps on every interrupt
static volatile bool s_handoff;           // drained; keep MODE_SPK for SPEAK→MIC
static speaker_idle_cb_t s_idle_cb;
static WORD_ALIGNED_ATTR uint8_t s_wire[PLAY_REQ_MAX + 64];   // DMA scratch, internal RAM

static size_t waiting(void)
{
    UBaseType_t w = 0;
    if (!s_ring) return 0;
    vRingbufferGetInfo(s_ring, NULL, NULL, NULL, NULL, &w);
    return w;
}

// 48 kHz mono -> 48 kHz stereo (L=R) big-endian int16, in the ring.
size_t speaker_play(const int16_t *pcm, size_t samples)
{
    enum { BLK = 256 };
    uint8_t wire[BLK * 4];               // 4 bytes per mono sample (stereo int16)
    size_t done = 0;
    if (!s_ring) return 0;
    while (done < samples) {
        size_t n = samples - done; if (n > BLK) n = BLK;
        for (size_t i = 0; i < n; i++) {
            uint16_t v = (uint16_t)pcm[done + i];
            uint8_t hi = v >> 8, lo = v & 0xFF;
            uint8_t *q = &wire[i * 4];
            q[0] = hi; q[1] = lo; q[2] = hi; q[3] = lo;
        }
        if (xRingbufferSend(s_ring, wire, n * 4, 0) != pdTRUE)
            break;                       // ring full; caller throttles
        done += n;
    }
    if (done) s_last_enqueue_us = esp_timer_get_time();
    return done;
}

void speaker_flush(void)
{
    size_t sz;
    void *p;
    if (!s_ring) return;
    while ((p = xRingbufferReceiveUpTo(s_ring, &sz, 0, PLAY_RING_BYTES)) != NULL)
        vRingbufferReturnItem(s_ring, p);
}

void speaker_tone(unsigned hz, unsigned ms, int16_t amplitude)
{
    size_t n = 48000u * ms / 1000;
    int16_t *buf = malloc(n * 2);
    if (!buf) return;
    for (size_t i = 0; i < n; i++)
        buf[i] = (int16_t)(amplitude * sin(2 * M_PI * hz * i / 48000.0));
    size_t off = 0;
    while (off < n) {
        size_t d = speaker_play(buf + off, n - off);
        off += d;
        if (!d) vTaskDelay(pdMS_TO_TICKS(10));   // ring full: let it drain
    }
    free(buf);
}

// Pull `want` bytes; zero-fill (silence) whatever is missing.
static void pull(uint8_t *buf, size_t want)
{
    size_t got = 0;
    while (got < want) {
        size_t sz = 0;
        void *p = xRingbufferReceiveUpTo(s_ring, &sz, 0, want - got);
        if (!p) break;
        memcpy(buf + got, p, sz);
        vRingbufferReturnItem(s_ring, p);
        got += sz;
    }
    if (got < want) memset(buf + got, 0, want - got);
}

static void enter_speak(void)
{
    if (k210_link_state() == LINK_LISTEN) {      // half-duplex: stop the mic first
        k210_uart_send_mode(K210_MODE_IDLE);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_handoff = false;
    k210_uart_send_mode(K210_MODE_SPK);
    k210_link_set_state(LINK_SPEAK);
    microphone_mark_closed();
    s_ignored = 0;
    s_last_req_us = esp_timer_get_time();
    ESP_LOGI(TAG, "MODE_SPK (%u bytes buffered)", (unsigned)waiting());
}

static void leave_speak(void)
{
    // Allow Xiaozhi (etc.) to jump SPEAK→MIC without an IDLE gap — IDLE after
    // MODE_SPK often leaves the K210 ignoring a later MODE_MIC.
    if (s_idle_cb && s_idle_cb()) {
        // Stay LINK_SPEAK so recover can SPEAK→MIC. Ignore further SPK_REQs.
        s_handoff = true;
        ESP_LOGI(TAG, "speaker drained -> handed off (keep SPK for MIC)");
        return;
    }
    s_handoff = false;
    k210_uart_send_mode(K210_MODE_IDLE);
    k210_link_set_state(LINK_IDLE);
    ESP_LOGI(TAG, "speaker drained -> MODE_IDLE (wake word armed)");
}

void speaker_interrupt(const char *why)
{
    s_serial++;
    s_interrupts++;
    s_handoff = false;
    speaker_flush();
    if (k210_link_state() == LINK_SPEAK) {
        k210_uart_send_mode(K210_MODE_IDLE);
        k210_link_set_state(LINK_IDLE);
    }
    ESP_LOGW(TAG, "playback cut (%s) -> MODE_IDLE", why);
}

uint32_t speaker_serial(void) { return s_serial; }
bool speaker_ready(void) { return s_ring != NULL; }
bool speaker_busy(void)
{
    // Handoff: ring empty, waiting for SPEAK→MIC — not "playing".
    if (s_handoff) return false;
    return k210_link_state() == LINK_SPEAK || waiting() > 0;
}
void speaker_end_handoff(void) { s_handoff = false; }
void speaker_set_idle_cb(speaker_idle_cb_t cb) { s_idle_cb = cb; }

void speaker_on_spk_req(uint16_t req)
{
    s_requests++;
    s_last_req_us = esp_timer_get_time();
    if (k210_link_state() != LINK_SPEAK && !s_handoff) return;
    size_t n = req ? req : SPK_BLOCK;        // always 0x3000 in captures
    if (n > PLAY_REQ_MAX) n = PLAY_REQ_MAX;

    // After drain: keep feeding silence so the K210 SPK task doesn't hang
    // (ignoring SPK_REQ wedges UART and forces a soft-reset that looks like reboot).
    if (s_handoff) {
        memset(s_wire, 0, n);
        esp_rom_delay_us(1500);
        k210_spi_write(s_wire, (uint16_t)n, 0, 1000);
        return;
    }

    size_t avail = waiting();
    int64_t now = esp_timer_get_time();
    if (avail >= n) {
        // full block
    } else if (avail > 0) {
        // Partial block: pad with silence and answer NOW. A live stream (Xiaozhi
        // TTS) arrives ~real time, so ignoring requests while it trickles in
        // starves the K210 speaker task and wedges its UART afterwards
        // (plain buffered playback never does).
    } else if (now - s_last_enqueue_us < SPK_LIVE_US) {
        // Stream still live but momentarily empty: keep the K210 fed with silence.
        s_ignored = 0;
    } else {
        if (++s_ignored >= SPK_DRAIN_REQS) leave_speak();
        return;
    }
    s_ignored = 0;
    pull(s_wire, n);
    esp_rom_delay_us(1500);                  // stock: ~1.9 ms from SPK_REQ to the command
    k210_spi_write(s_wire, (uint16_t)n, 0, 1000);
}

void speaker_tick(int64_t now)
{
    if (k210_link_booting()) return;
    if (k210_link_state() == LINK_SPEAK) {
        // The K210 stops asking for blocks when it hears its wake word mid-speech
        // (or gets lost); without this we would stay in MODE_SPK forever.
        if (!s_handoff && now - s_last_req_us > SPK_REQ_TIMEOUT_US)
            speaker_interrupt("no SPK_REQ for 1.5 s");
        return;
    }
    // Speaker audio queued: enter MODE_SPK once enough is buffered, or when a
    // short clip has fully arrived.
    size_t w = waiting();
    if (w >= SPK_START_BYTES || (w > 0 && now - s_last_enqueue_us > SPK_STALL_US))
        enter_speak();
}

void speaker_get_stats(speaker_stats_t *out)
{
    out->queued_samples = waiting() / 4;     // stereo frames = mono samples
    out->requests = s_requests;
    out->interrupts = s_interrupts;
}

esp_err_t speaker_init(void)
{
    s_ring = xRingbufferCreateWithCaps(PLAY_RING_BYTES, RINGBUF_TYPE_BYTEBUF,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        ESP_LOGW(TAG, "no PSRAM for the %u KB play ring, using %u KB internal", PLAY_RING_BYTES / 1024, PLAY_RING_SMALL / 1024);
        s_ring = xRingbufferCreate(PLAY_RING_SMALL, RINGBUF_TYPE_BYTEBUF);
    }
    if (!s_ring) { ESP_LOGE(TAG, "play ring alloc failed: no speaker"); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}
