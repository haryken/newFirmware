// microphone.c — see microphone.h.
#include "microphone.h"
#include "k210_link.h"
#include "k210_uart.h"
#include "k210_spi.h"
#include "speaker.h"
#include "sdkconfig.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "mic";

#define MIC_BYTES       1024            // 512 samples @ 16 kHz per MIC_READY
#define MIC_REQ_MAX     8192

// Utterance window tuning (see microphone.h).
#define VOICE_PEAK      2600            // |sample| above this counts as voice (room noise ~500-2000)
#define HANGOVER_US     1500000         // voice this recent at a K210 cut -> extend the window
#define END_SILENCE_US  1000000         // quiet this long in an extended window -> end of turn
#define SESSION_MAX_US  15000000
#define MIC_STALL_US    6000000         // post-TTS MIC_READY can lag

static microphone_cb_t s_cb;
static microphone_event_cb_t s_event_cb;
static microphone_stats_t s_st;

static volatile int64_t s_reopen_at;    // esp_timer us; 0 = nothing pending
static volatile bool s_reopen_wake;     // open as a wake turn (face anim, no 4B)
static volatile bool s_in_session;
static bool s_sess_extended;            // at least one follow-up window opened
static int64_t s_sess_start_us, s_last_voice_us, s_last_mic_us;
static int64_t s_grace_until_us;        // END_OF_SPEECH extends listen until this
static volatile bool s_heard_voice;     // real speech seen in this utterance (ends grace)
static volatile int s_win_peak;         // loudest sample since the last utterance end (debug)

static WORD_ALIGNED_ATTR uint8_t s_buf[MIC_REQ_MAX + 64];   // DMA scratch, internal RAM

void microphone_init(microphone_cb_t cb) { s_cb = cb; }
void microphone_set_event_cb(microphone_event_cb_t cb) { s_event_cb = cb; }
void microphone_mark_closed(void)
{
    // Speaker took the link — drop the utterance quietly (no END_OF_SPEECH).
    // Otherwise after MODE_SPK drains, the mic watchdog fires "no mic data"
    // and Xiaozhi follow-up gets an immediate listen-stop.
    s_st.open = false;
    s_in_session = false;
    s_reopen_at = 0;
    s_grace_until_us = 0;
    s_heard_voice = false;
}

void microphone_set_listen_grace_ms(uint32_t ms)
{
    s_grace_until_us = ms ? (esp_timer_get_time() + (int64_t)ms * 1000) : 0;
    s_heard_voice = false;
}

void microphone_get_stats(microphone_stats_t *out)
{
    *out = s_st;
    out->in_session = s_in_session;
}

static void schedule_open(uint32_t delay_ms, bool wake)
{
    s_reopen_wake = wake;
    s_reopen_at = esp_timer_get_time() + (int64_t)delay_ms * 1000;
}

// wake = true: the stock wake turn (voice_wake_up face, then MODE_MIC).
// wake = false: a follow-up window (4B then MODE_MIC), also the web WAV capture.
static void start_listen(bool wake)
{
    if (k210_link_state() == LINK_SPEAK) return;         // half-duplex: never while speaking
    if (wake) {
#if CONFIG_EMO_SD_FACES
        k210_uart_send_ext(0x02, "[0,[[\"/spiffs/avi/voice_wake_up.avi\",[[0,\"/spiffs/mp3/voice_wake_up_000.mp3\"]]]]]");
#endif
    } else {
        k210_uart_send_short(0x4B, 0x00, 0x00, 0x00);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    k210_uart_send_mode(K210_MODE_MIC);
    k210_link_set_state(LINK_LISTEN);
    s_st.open = true;
    s_last_mic_us = esp_timer_get_time();
    if (K210_LINK_DEBUG >= 2) ESP_LOGI(TAG, "MODE_MIC sent (%s)", wake ? "wake" : "follow-up");
}

esp_err_t microphone_open(void)
{
    if (k210_link_booting() || k210_link_state() == LINK_SPEAK) return ESP_ERR_INVALID_STATE;
    // Treat as a real utterance so END_OF_SPEECH / silence end the turn
    // (needed for Xiaozhi follow-up after TTS — not just web WAV capture).
    s_in_session = true;
    s_sess_extended = false;
    s_sess_start_us = s_last_voice_us = esp_timer_get_time();
    s_last_mic_us = s_sess_start_us;
    start_listen(false);
    return ESP_OK;
}

esp_err_t microphone_open_followup(void)
{
    if (k210_link_booting()) return ESP_ERR_INVALID_STATE;

    // Prefer SPEAK→MIC (no IDLE). If already idle, poke MIC (+ 4B like mid-turn).
    bool from_spk = (k210_link_state() == LINK_SPEAK);
    if (from_spk) {
        k210_uart_send_mode(K210_MODE_MIC);
    } else {
        k210_uart_send_short(0x4B, 0x00, 0x00, 0x00);
        vTaskDelay(pdMS_TO_TICKS(20));
        k210_uart_send_mode(K210_MODE_MIC);
    }
    k210_link_set_state(LINK_LISTEN);
    speaker_end_handoff();

    s_in_session = true;
    s_sess_extended = false;
    s_st.open = true;
    s_sess_start_us = s_last_voice_us = s_last_mic_us = esp_timer_get_time();
    ESP_LOGI(TAG, "follow-up MODE_MIC armed (from %s)", from_spk ? "spk" : "idle");
    return ESP_OK;
}

static void end_session(const char *why)
{
    s_in_session = false;
    s_st.open = false;
    if (k210_link_state() == LINK_LISTEN) {              // stop a window we extended ourselves
        k210_uart_send_mode(K210_MODE_IDLE);
        k210_link_set_state(LINK_IDLE);
    }
    ESP_LOGI(TAG, "utterance over (%s, %.1f s, heard_voice=%d maxpeak=%d) -> MODE_IDLE", why,
             (esp_timer_get_time() - s_sess_start_us) / 1e6, (int)s_heard_voice, (int)s_win_peak);
    s_win_peak = 0;
    s_heard_voice = false;
    if (s_event_cb) s_event_cb(MIC_EV_END_OF_SPEECH);
}

void microphone_on_mic_ready(uint16_t val)
{
    uint16_t n = val ? val : MIC_BYTES;
    if (n > MIC_REQ_MAX) n = MIC_REQ_MAX;
    if (k210_link_state() == LINK_IDLE) k210_link_set_state(LINK_LISTEN);   // the K210 is streaming, whatever we think
    esp_rom_delay_us(500);                               // stock: ~0.64 ms from MIC_READY to the command
    k210_spi_read(s_buf, n, 1, 600);
    // Verified layout (FIRMWARE_HANDOFF.md §5.1): each 4-byte word = two
    // big-endian samples, later one first == swap4 then read as LE16.
    k210_swap4(s_buf, (n + 3) & ~3u);
    size_t samples = n / 2;
    const int16_t *pcm = (const int16_t *)s_buf;
    int peak = 0;
    for (size_t i = 0; i < samples; i++) {
        int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak) peak = a;
    }
    s_st.peak = (uint16_t)peak;
    s_st.samples += samples;
    s_st.open = true;
    s_last_mic_us = esp_timer_get_time();
    if (s_in_session && (s_st.samples % 16000) < samples)
        ESP_LOGI(TAG, "mic streaming peak=%d samples=%u", peak, (unsigned)s_st.samples);
    if (s_cb) s_cb(pcm, samples);

    if (s_in_session) {
        int64_t now = esp_timer_get_time();
        if (peak > s_win_peak) s_win_peak = peak;
        if (peak > VOICE_PEAK) { s_last_voice_us = now; s_heard_voice = true; }
        bool grace = s_grace_until_us && now < s_grace_until_us;
        if (now - s_sess_start_us > SESSION_MAX_US)
            end_session("max length");
        else if (s_sess_extended && (s_heard_voice || !grace) &&
                 now - s_last_voice_us > END_SILENCE_US)
            end_session("silence");     // user spoke, then paused -> send to server now
    }
}

void microphone_on_voice_event(uint8_t p1, uint8_t p2)
{
    if (p1 == 1) {                                       // WAKE: start an utterance
        s_st.wake_events++;
        ESP_LOGI(TAG, "K210: wake word (p2=%02X) -> listening", p2);
        if (k210_link_state() == LINK_SPEAK) {
            // Barge-in: the wake word is still armed in MODE_SPK and the K210 stops
            // pulling audio once it fires. Cut the reply and listen instead.
            speaker_interrupt("wake word while speaking");
        }
        if (s_in_session) {                              // K210 re-fired inside our window
            ESP_LOGW(TAG, "wake while an utterance is running, ignored");
            return;
        }
        k210_uart_send_short(0x09, 0x00, 0x00, 0x00);    // STOP_ANIM, as the stock does
        s_in_session = true;
        s_last_mic_us = esp_timer_get_time();
        s_sess_extended = false;
        s_sess_start_us = s_last_voice_us = esp_timer_get_time();
        if (s_event_cb) s_event_cb(MIC_EV_WAKE);
        schedule_open(60, true);                         // wake face + MODE_MIC
        return;
    }
    // END_OF_SPEECH: the K210 already stopped the mic
    if (k210_link_state() == LINK_SPEAK) return;         // stale: MODE_IDLE here would cut the speaker
    s_st.open = false;
    if (k210_link_state() == LINK_LISTEN) k210_link_set_state(LINK_IDLE);
    if (!s_in_session) {                                 // a web WAV-capture window: just re-arm
        k210_uart_send_mode(K210_MODE_IDLE);
        return;
    }
    int64_t now = esp_timer_get_time();
    if (!s_heard_voice && s_grace_until_us && now < s_grace_until_us) {
        s_sess_extended = true;
        s_last_voice_us = now;
        s_sess_start_us = now;   // don't hit SESSION_MAX mid-grace
        ESP_LOGI(TAG, "K210 cut during listen grace -> re-open MIC");
        schedule_open(20, false);
    } else if (now - s_sess_start_us < SESSION_MAX_US && now - s_last_voice_us < HANGOVER_US) {
        // Voice was heard moments ago: the K210 cut on a pause. Keep listening.
        s_sess_extended = true;
        ESP_LOGI(TAG, "K210 cut after a pause -> extending the listen window");
        schedule_open(20, false);                        // follow-up window (4B + MODE_MIC)
    } else {
        s_grace_until_us = 0;
        s_heard_voice = false;
        end_session("K210 end of speech");
    }
}

void microphone_tick_reopen(int64_t now)
{
    // Pending mic open, scheduled by WAKE (so STOP_ANIM lands first) or by a cut.
    if (s_reopen_at && now >= s_reopen_at) {
        s_reopen_at = 0;
        if (!k210_link_booting() && k210_link_state() != LINK_SPEAK) start_listen(s_reopen_wake);
    }
}

void microphone_tick_watchdog(int64_t now)
{
    // A session the K210 never closes (no END_OF_SPEECH, no mic frames) must not
    // keep the mic and the host stream open forever.
    if (s_in_session && k210_link_state() != LINK_SPEAK && !k210_link_booting() && !s_reopen_at) {
        if (now - s_sess_start_us > SESSION_MAX_US + 2000000) end_session("timeout");
        else if (now - s_last_mic_us > MIC_STALL_US) end_session("no mic data");
    }
}
