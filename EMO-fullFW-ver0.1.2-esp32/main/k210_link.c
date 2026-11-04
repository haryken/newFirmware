// k210_link.c — see k210_link.h.
#include "k210_link.h"
#include "k210_uart.h"
#include "k210_spi.h"
#include "k210_file.h"
#include "k210_boot.h"
#include "microphone.h"
#include "speaker.h"
#include "behavior.h"
#include "sdkconfig.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "k210";

#define PIN_RESET   CONFIG_EMO_K210_RESET_GPIO
#define PIN_PWR     CONFIG_EMO_PWR_EN_GPIO

static volatile link_state_t s_state;
static volatile bool s_booting;
static volatile bool s_ack_seen, s_anim_done_seen, s_version_seen;
static char s_version[16];
static uint32_t s_rx_frames, s_bad_cksum;
static int64_t s_last_rx_us;

// ---- state / flags ----
link_state_t k210_link_state(void) { return s_state; }
void k210_link_set_state(link_state_t st) { s_state = st; }
bool k210_link_booting(void) { return s_booting; }
void k210_link_set_booting(bool on) { s_booting = on; }

// Poll a flag set by the RX task. Not for use from the RX task itself.
static bool wait_flag(volatile bool *flag, uint32_t timeout_ms)
{
    for (uint32_t t = 0; t < timeout_ms; t += 5) {
        if (*flag) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return *flag;
}

void k210_link_clear_ack(void) { s_ack_seen = false; }
bool k210_link_wait_ack(uint32_t ms) { return wait_flag(&s_ack_seen, ms); }
void k210_link_clear_anim_done(void) { s_anim_done_seen = false; }
void k210_link_set_anim_done(void) { s_anim_done_seen = true; }
bool k210_link_wait_anim_done(uint32_t ms) { return wait_flag(&s_anim_done_seen, ms); }
void k210_link_clear_version(void) { s_version_seen = false; s_version[0] = '\0'; }
bool k210_link_version_seen(void) { return s_version_seen; }
const char *k210_link_version(void) { return s_version; }

void k210_link_reset_pulse(uint32_t low_ms)
{
    ESP_LOGI(TAG, "RESETN pulse %u ms", (unsigned)low_ms);
    gpio_set_level(PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(low_ms));
    gpio_set_level(PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

bool k210_link_ping(uint32_t timeout_ms)
{
    if (s_booting) return false;
    s_ack_seen = false;
    s_anim_done_seen = false;
    // STOP_ANIM is answered with ANIM_DONE (55 AA 08), NOT the FE ACK that
    // extended frames get. Waiting for FE here made a healthy K210 look dead
    // and caused a pointless ~4 s RESETN (black screen) after every reply.
    k210_uart_send_short(0x09, 0x00, 0x00, 0x00);
    for (uint32_t t = 0; t < timeout_ms; t += 5) {
        if (s_anim_done_seen || s_ack_seen) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return s_anim_done_seen || s_ack_seen;
}

bool k210_link_recover_after_spk(void)
{
    if (s_booting) return false;

    // Ping only — do NOT send MODE_IDLE here. IDLE right after MODE_SPK often
    // kills UART ACKs and leaves wake/MIC dead until RESETN.
    if (k210_link_ping(300)) {
        ESP_LOGI(TAG, "post-spk: UART ok (state=%u)", (unsigned)s_state);
        return true;
    }

    ESP_LOGW(TAG, "post-spk: UART dead after MODE_SPK — soft reset K210");
    microphone_mark_closed();
    s_state = LINK_IDLE;
    s_booting = true;   // block mic/spk briefly
    k210_link_clear_version();
    k210_link_reset_pulse(1);
    // Fast path: full boot uses 2 s + long VERSION poll; post-TTS must feel
    // like a blink, not a reboot.
    vTaskDelay(pdMS_TO_TICKS(700));
    k210_uart_send_short(0x34, 0x00, 0x00, 0x00);   // boot gate

    int64_t t0 = esp_timer_get_time();
    while (!s_version_seen && esp_timer_get_time() - t0 < 4000000) {
        k210_uart_send_short(0x69, 0x01, 0x00, 0x00);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    vTaskDelay(pdMS_TO_TICKS(150));

    // Minimal stage so MIC / wake work (skip 6A/6C/74 — not required to listen).
    static const uint8_t stage[][4] = {
        { 0x70, 0x00, 0x00, 0x00 }, { 0x38, 0x01, 0x00, 0x00 },
        { 0x06, 0x01, 0x00, 0x00 }, { 0x17, 0x68, 0x00, 0x00 },
    };
    for (size_t i = 0; i < sizeof(stage) / sizeof(stage[0]); i++) {
        k210_uart_send_short(stage[i][0], stage[i][1], stage[i][2], stage[i][3]);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    k210_uart_send_mode(K210_MODE_IDLE);
    s_state = LINK_IDLE;
    s_booting = false;
    ESP_LOGI(TAG, "post-spk: K210 recovered fast (ver=%s)",
             s_version[0] ? s_version : "?");
    return s_version_seen;
}

void k210_link_power_off(void)
{
    ESP_LOGW(TAG, "power off: power-enable GPIO%d -> LOW", PIN_PWR);
    vTaskDelay(pdMS_TO_TICKS(100));                  // let the HTTP reply and the log out
    gpio_set_level(PIN_PWR, 0);
}

// ---- raw command access (k210_controls.c) ----
esp_err_t k210_send_short(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3)
{
    if (s_booting) return ESP_ERR_INVALID_STATE;
    k210_uart_send_short(cmd, p1, p2, p3);
    ESP_LOGI(TAG, "ctl: 55 AA %02X %02X %02X %02X %02X", cmd, p1, p2, p3, (uint8_t)(cmd + p1 + p2 + p3 - 1));
    return ESP_OK;
}

esp_err_t k210_send_ext(uint8_t sub, const char *payload, bool *acked)
{
    if (s_booting) return ESP_ERR_INVALID_STATE;
    if (!payload || strlen(payload) + 1 > 255) return ESP_ERR_INVALID_SIZE;
    s_ack_seen = false;
    k210_uart_send_ext(sub, payload);
    bool ok = wait_flag(&s_ack_seen, 500);         // stock ESP32 waits 500 ms for FE
    if (acked) *acked = ok;
    ESP_LOGI(TAG, "ctl: ext sub %02X \"%s\" -> %s", sub, payload, ok ? "ACK" : "no ACK");
    return ESP_OK;
}

// ---- RX dispatch ----
static void handle_short(uint8_t cmd, uint8_t p1, uint8_t p2, uint8_t p3)
{
    uint16_t val = (uint16_t)p2 << 8 | p1;
    switch (cmd) {
    case 0x00: speaker_on_spk_req(val); break;            // SPK_REQ: room for one block
    case 0x01: microphone_on_mic_ready(val); break;       // MIC_READY: a mic frame, read it now
    case 0x04: k210_file_on_chunk(val); break;            // FILE_CHUNK (ext 0x03 read)
    case 0x05: k210_file_on_done(); break;                // file read finished
    case 0x08: s_anim_done_seen = true; break;            // ANIM_DONE
    case 0x0B: microphone_on_voice_event(p1, p2); break;  // WAKE (p1=1) / END_OF_SPEECH (p1=0)
    case 0x0D: break;                                     // mic level / direction, informational
    case 0x47: ESP_LOGI(TAG, "K210: ready"); break;
    case 0xFE: s_ack_seen = true; break;                  // ACK of an extended frame
    case 0x3F:
        ESP_LOGI(TAG, "K210: yolo class=%u x=%u y=%u", p1, p2, p3);
        behavior_on_yolo(p1, p2, p3);
        break;
    case 0x68: ESP_LOGW(TAG, "K210: error code %u", p1); break;
    default: break;
    }
}

// Extended frame from the K210: payload (len bytes incl. NUL) + crc8.
static void handle_long(uint16_t len, uint8_t sub)
{
    uint8_t buf[64];
    size_t keep = 0;
    uint16_t left = (uint16_t)(len + 1);
    while (left) {
        uint8_t junk[64];
        int m = k210_uart_read(junk, left > sizeof(junk) ? sizeof(junk) : left, 50);
        if (m <= 0) break;
        if (K210_LINK_DEBUG >= 2) ESP_LOG_BUFFER_HEX(TAG, junk, m);
        size_t room = sizeof(buf) - keep;
        size_t c = (size_t)m < room ? (size_t)m : room;
        memcpy(buf + keep, junk, c); keep += c;
        left -= m;
    }
    if (sub != 0x69) {                   // anything else the K210 reports, as text
        char txt[64]; size_t k = keep < sizeof(txt) - 1 ? keep : sizeof(txt) - 1;
        for (size_t i = 0; i < k; i++) txt[i] = (buf[i] >= 0x20 && buf[i] < 0x7F) ? (char)buf[i] : (buf[i] ? '.' : ' ');
        txt[k] = 0;
        ESP_LOGW(TAG, "K210 ext %02X len %u: %s", sub, (unsigned)len, txt);
    }
    if (sub == 0x69 && len >= 1) {       // VERSION, e.g. "3.1.0"
        size_t n = len - 1; if (n > sizeof(s_version) - 1) n = sizeof(s_version) - 1;
        if (n > keep) n = keep;
        memcpy(s_version, buf, n); s_version[n] = '\0';
        s_version_seen = true;
        ESP_LOGI(TAG, "K210: version %s", s_version);
    }
}

static void rx_task(void *arg)
{
    enum { ST_55, ST_AA, ST_BODY } st = ST_55;
    uint8_t body[8];
    int bi = 0;
    for (;;) {
        uint8_t c;
        int n = k210_uart_read(&c, 1, 100);
        int64_t now = esp_timer_get_time();

        // Periodic work, every byte or every 100 ms when the line is quiet:
        // mic re-open scheduled by WAKE, then the speaker entering MODE_SPK when
        // audio is queued, then the stalled-session watchdog.
        microphone_tick_reopen(now);
        speaker_tick(now);
        microphone_tick_watchdog(now);
        if (n <= 0) continue;

        switch (st) {
        case ST_55:
            if (c == 0x55) st = ST_AA;
            else if (K210_LINK_DEBUG >= 2) ESP_LOGI(TAG, "RX junk %02X", c);
            break;
        case ST_AA:
            if (c == 0xAA) { st = ST_BODY; bi = 0; }
            else if (c == 0x55) { st = ST_AA; }
            else { st = ST_55; if (K210_LINK_DEBUG >= 2) ESP_LOGI(TAG, "RX junk 55 %02X", c); }
            break;
        case ST_BODY:
            body[bi++] = c;
            if (bi == 5) {
                uint8_t cmd = body[0], p1 = body[1], p2 = body[2], p3 = body[3], ck = body[4];
                uint8_t calc = (uint8_t)(cmd + p1 + p2 + p3 - 1);
                if (K210_LINK_DEBUG >= 2 || (K210_LINK_DEBUG == 1 && cmd != 0x00 && cmd != 0x01 && cmd != 0x0D))
                    ESP_LOGI(TAG, "RX 55 AA %02X %02X %02X %02X %02X%s",
                             cmd, p1, p2, p3, ck, calc == ck ? "" : "  BAD CHECKSUM");
                if (calc != ck) {
                    s_bad_cksum++;
                } else {
                    s_last_rx_us = now;
                    s_rx_frames++;
                    if (cmd != 0xFF) handle_short(cmd, p1, p2, p3);
                    else handle_long((uint16_t)p2 << 8 | p1, p3);
                }
                st = ST_55;
            }
            break;
        }
    }
}

void k210_get_stats(k210_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    out->rx_frames = s_rx_frames;
    out->bad_cksum = s_bad_cksum;
    k210_spi_counts(&out->spi_reads, &out->spi_writes);
    microphone_stats_t m; microphone_get_stats(&m);
    out->mic_samples = m.samples;
    out->mic_peak = m.peak;
    out->mic_open = m.open;
    out->wake_events = m.wake_events;
    speaker_stats_t s; speaker_get_stats(&s);
    out->play_queued = s.queued_samples;
    out->play_requests = s.requests;
    out->state = (uint8_t)s_state;
    strlcpy(out->version, s_version, sizeof(out->version));
    out->linked = (esp_timer_get_time() - s_last_rx_us) < 15000000;
    out->boot_done = k210_boot_done();
}

esp_err_t k210_link_start(void)
{
    // Power-enable first: HIGH keeps the robot on once the button is released.
    gpio_config_t ctl = {
        .pin_bit_mask = (1ULL << PIN_PWR) | (1ULL << PIN_RESET),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&ctl));
    gpio_set_level(PIN_PWR, 1);
    gpio_set_level(PIN_RESET, 1);

#if CONFIG_SPIRAM && defined(CONFIG_D0WD_PSRAM_CLK_IO)
    // On WROVER modules these GPIOs are wired to the PSRAM; claiming them for
    // the K210 kills PSRAM and the next PSRAM access panics.
    const int pins[] = { CONFIG_EMO_K210_UART_TX, CONFIG_EMO_K210_UART_RX, CONFIG_EMO_SPI_SCLK,
                         CONFIG_EMO_SPI_MOSI, CONFIG_EMO_SPI_MISO, CONFIG_EMO_SPI_CS, PIN_RESET, PIN_PWR };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if (pins[i] == CONFIG_D0WD_PSRAM_CLK_IO || pins[i] == CONFIG_D0WD_PSRAM_CS_IO) {
            ESP_LOGE(TAG, "GPIO%d is used by PSRAM; fix the K210 pins in menuconfig. "
                          "K210 link disabled.", pins[i]);
            return ESP_ERR_INVALID_ARG;
        }
    }
#endif

    esp_err_t e;
    if ((e = k210_uart_init()) != ESP_OK) { ESP_LOGE(TAG, "uart: %s", esp_err_to_name(e)); return e; }
    if ((e = k210_spi_init()) != ESP_OK)  { ESP_LOGE(TAG, "spi: %s", esp_err_to_name(e)); return e; }

    // Mic callback + face JSON live here; keep headroom (was 6 KB — Opus overflow).
    xTaskCreatePinnedToCore(rx_task, "k210_link", 10240, NULL, 12, NULL, 1);
    ESP_LOGI(TAG, "K210 link up: RST=%d, PWR=%d", PIN_RESET, PIN_PWR);
    return ESP_OK;
}
