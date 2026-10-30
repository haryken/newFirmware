// host_stream.c — see host_stream.h.
#include "host_stream.h"
#include "settings.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_crc.h"

static const char *TAG = "host";

// The server address is a setting (web page / NVS; menuconfig gives the default)
// and is read again for every connection, so a change applies at the next utterance.
static char s_host_ip[16];
static uint16_t s_host_port;

static void load_host(void)
{
    emo_settings_t st; settings_get_copy(&st);
    strlcpy(s_host_ip, st.host_ip, sizeof(s_host_ip));
    s_host_port = st.host_port;
}

// 2 MB PSRAM backlog ≈ 65 s of 16k mono int16 (an utterance is <= 17 s). Leaves
// room in the 4 MB PSRAM for the speaker ring and the HTTP buffers; falls back
// to a small internal ring only if PSRAM is absent. A plain contiguous ring
// indexed by absolute byte offset.
#define BACKLOG_TRY   (2 * 1024 * 1024)
#define BACKLOG_MIN   (64 * 1024)

static uint8_t *s_buf;
static size_t   s_cap;
static uint64_t s_head;        // absolute offset of next byte to write (== fed)
static uint64_t s_acked;       // absolute offset host has committed
static SemaphoreHandle_t s_lock;

static volatile bool s_session;
static volatile bool s_connected;
static host_stream_stats_t s_stats;
static host_stream_monitor_cb_t s_mon;
static void *s_mon_ctx;

void host_stream_set_monitor(host_stream_monitor_cb_t cb, void *ctx)
{
    s_mon = cb; s_mon_ctx = ctx;
}

static void update_meter(const int16_t *pcm, size_t n)
{
    int16_t peak = 0;
    double acc = 0;
    for (size_t i = 0; i < n; i++) {
        int16_t v = pcm[i];
        int16_t a = v < 0 ? -v : v;
        if (a > peak) peak = a;
        acc += (double)v * v;
    }
    s_stats.peak = peak;
    s_stats.rms = n ? (float)sqrt(acc / n) : 0;
}

void host_stream_feed(const int16_t *pcm, size_t samples)
{
    if (s_mon) s_mon(pcm, samples, s_mon_ctx);
    update_meter(pcm, samples);
    if (!s_session) return;              // only the utterance goes to the host
    if (!s_buf || !s_cap) return;        // backlog alloc failed: degrade, don't crash

    size_t bytes = samples * 2;
    const uint8_t *src = (const uint8_t *)pcm;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // If this would overrun unacked data, drop oldest unacked (bounded loss).
    uint64_t unacked = s_head - s_acked;
    if (unacked + bytes > s_cap) {
        uint64_t drop = (unacked + bytes) - s_cap;
        s_acked += drop;               // abandon oldest; counts as overflow
        s_stats.overflow += drop;
    }
    for (size_t i = 0; i < bytes; i++)
        s_buf[(s_head + i) % s_cap] = src[i];
    s_head += bytes;
    s_stats.fed = s_head;
    xSemaphoreGive(s_lock);
}

void host_stream_session_begin(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_acked = s_head;                    // start the utterance from a clean cursor
    s_stats.acked = s_acked;
    s_stats.sessions++;
    xSemaphoreGive(s_lock);
    s_session = true;
    s_stats.session = true;
    load_host();
    ESP_LOGI(TAG, "session %u: streaming to %s:%u", (unsigned)s_stats.sessions, s_host_ip, (unsigned)s_host_port);
}

void host_stream_session_end(void)
{
    if (!s_session) return;
    s_session = false;
    s_stats.session = false;
    ESP_LOGI(TAG, "session end, flushing");
}

bool host_stream_in_session(void) { return s_session; }

// Copy [off, off+len) out of the ring into dst. Caller ensures range is valid.
static void ring_read(uint64_t off, uint8_t *dst, size_t len)
{
    for (size_t i = 0; i < len; i++)
        dst[i] = s_buf[(off + i) % s_cap];
}

static bool send_all(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        int w = send(fd, b, n, 0);
        if (w <= 0) return false;
        b += w; n -= w;
    }
    return true;
}

// Reader side: parse "ACK0" + off(8) messages to advance s_acked.
static void pump_acks(int fd)
{
    static uint8_t acc[32];
    static int have;
    uint8_t tmp[32];
    int r = recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
    if (r <= 0) return;
    for (int i = 0; i < r && have < (int)sizeof(acc); i++) acc[have++] = tmp[i];
    while (have >= 12) {
        if (memcmp(acc, "ACK0", 4) == 0) {
            uint64_t off;
            memcpy(&off, acc + 4, 8);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (off > s_acked && off <= s_head) s_acked = off;
            s_stats.acked = s_acked;
            xSemaphoreGive(s_lock);
            memmove(acc, acc + 12, have - 12);
            have -= 12;
        } else {
            memmove(acc, acc + 1, --have);   // resync
        }
    }
}

static int connect_host(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(s_host_port);
    if (inet_pton(AF_INET, s_host_ip, &a.sin_addr) != 1) { close(fd); return -1; }
    struct timeval tv = { .tv_sec = 3 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return -1; }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

// Discard anything the host never confirmed once the utterance is over.
static void drop_backlog(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_acked = s_head;
    s_stats.acked = s_acked;
    xSemaphoreGive(s_lock);
}

static void sender_task(void *arg)
{
    uint8_t chunk[1200];
    for (;;) {
        if (!s_session) {
            if (s_head != s_acked) drop_backlog();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int fd = connect_host();
        if (fd < 0) {
            load_host();                              // pick up an address changed meanwhile
            ESP_LOGW(TAG, "connect %s:%u failed, retrying", s_host_ip, (unsigned)s_host_port);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Resume from the last acked byte — nothing already-confirmed is resent,
        // and anything unacked is streamed again.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint64_t off = s_acked;
        uint64_t oldest = (s_head > s_cap) ? s_head - s_cap : 0;
        if (off < oldest) off = oldest;
        xSemaphoreGive(s_lock);

        // Hello
        uint8_t hello[4 + 1 + 4 + 1 + 1 + 8];
        memcpy(hello, "EMOA", 4);
        hello[4] = 1;
        uint32_t sr = 16000; memcpy(hello + 5, &sr, 4);
        hello[9] = 1; hello[10] = 16;
        memcpy(hello + 11, &off, 8);
        if (!send_all(fd, hello, sizeof(hello))) { close(fd); continue; }

        s_connected = true; s_stats.connected = true; s_stats.reconnects++;
        ESP_LOGI(TAG, "host connected, stream @ %llu", (unsigned long long)off);

        bool flushed = false;
        uint64_t sent0 = off;
        for (;;) {
            pump_acks(fd);
            // Clamp, size, and copy the chunk in ONE lock hold, so a concurrent
            // feed() can't overwrite the ring bytes between tagging `off` and
            // reading them.
            uint8_t payload[sizeof(chunk)];
            xSemaphoreTake(s_lock, portMAX_DELAY);
            uint64_t head = s_head;
            uint64_t oldest2 = (head > s_cap) ? head - s_cap : 0;
            if (off < oldest2) off = oldest2;   // backlog overran our cursor
            size_t avail = head - off;
            size_t len = avail; if (len > sizeof(chunk)) len = sizeof(chunk);
            if (len) ring_read(off, payload, len);
            xSemaphoreGive(s_lock);

            if (len == 0) {
                if (!s_session) {                // utterance over, everything sent:
                    int64_t t0 = esp_timer_get_time();   // give the host 300 ms to ACK
                    while (esp_timer_get_time() - t0 < 300000 && s_acked < off) {
                        pump_acks(fd);
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    flushed = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            uint8_t hdr[14];
            memcpy(hdr, &off, 8);
            uint16_t l16 = len; memcpy(hdr + 8, &l16, 2);
            uint32_t crc = esp_crc32_le(0, payload, len);
            memcpy(hdr + 10, &crc, 4);
            if (!send_all(fd, hdr, sizeof(hdr)) || !send_all(fd, payload, len)) break;
            off += len;
            s_stats.backlog = (uint32_t)(head - s_acked);
        }
        close(fd);
        s_connected = false; s_stats.connected = false;
        if (flushed) {
            ESP_LOGI(TAG, "utterance sent: %llu bytes (%llu ms)",
                     (unsigned long long)(off - sent0), (unsigned long long)((off - sent0) / 32));
            drop_backlog();
        } else {
            ESP_LOGW(TAG, "host disconnected%s", s_session ? ", reconnecting" : "");
        }
    }
}

void host_stream_get_stats(host_stream_stats_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_stats;
    out->backlog = (uint32_t)(s_head - s_acked);
    out->connected = s_connected;
    out->session = s_session;
    xSemaphoreGive(s_lock);
}

void host_stream_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cap = BACKLOG_TRY;
    s_buf = heap_caps_malloc(s_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        s_cap = BACKLOG_MIN;
        s_buf = malloc(s_cap);
    }
    if (!s_buf) { ESP_LOGE(TAG, "backlog alloc failed"); s_cap = 0; return; }
    load_host();
    ESP_LOGI(TAG, "backlog %u KB, host %s:%u", (unsigned)(s_cap / 1024), s_host_ip, (unsigned)s_host_port);
    xTaskCreatePinnedToCore(sender_task, "host_tx", 5120, NULL, 9, NULL, 0);
}
