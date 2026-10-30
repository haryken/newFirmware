// settings.c — NVS-backed configuration with a mutex-guarded live copy.
#include "settings.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>      // lwip: AF_INET + inet_pton (arpa/inet.h alone lacks them)
#include <arpa/inet.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

// Fallbacks for an sdkconfig generated before these options existed.
#ifndef CONFIG_EMO_MOTION_SPEED_PCT
#define CONFIG_EMO_MOTION_SPEED_PCT 100
#endif
#ifndef CONFIG_EMO_WIFI_SSID
#define CONFIG_EMO_WIFI_SSID ""
#endif
#ifndef CONFIG_EMO_WIFI_PASS
#define CONFIG_EMO_WIFI_PASS ""
#endif

static const char *TAG = "settings";
static const char *NS = "emo";
static SemaphoreHandle_t s_lock;
static emo_settings_t s_live;

static void set_defaults(emo_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    strncpy(s->wifi_ssid, CONFIG_EMO_WIFI_SSID, sizeof(s->wifi_ssid) - 1);   // blank = SoftAP fallback
    strncpy(s->wifi_pass, CONFIG_EMO_WIFI_PASS, sizeof(s->wifi_pass) - 1);
    strncpy(s->host_ip, CONFIG_EMO_HOST_IP, sizeof(s->host_ip) - 1);
    s->host_port = CONFIG_EMO_HOST_PORT;
    s->motion_speed_pct = CONFIG_EMO_MOTION_SPEED_PCT;
}

static void load_str(nvs_handle_t h, const char *k, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, k, dst, &len) != ESP_OK) {
        // keep whatever default is already in dst
    }
}

void settings_load(emo_settings_t *out)
{
    set_defaults(out);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no saved settings, using defaults");
        return;
    }
    load_str(h, "wifi_ssid", out->wifi_ssid, sizeof(out->wifi_ssid));
    load_str(h, "wifi_pass", out->wifi_pass, sizeof(out->wifi_pass));
    load_str(h, "host_ip", out->host_ip, sizeof(out->host_ip));
    uint16_t port;
    if (nvs_get_u16(h, "host_port", &port) == ESP_OK && port) out->host_port = port;
    uint16_t pct;
    if (nvs_get_u16(h, "motion_speed", &pct) == ESP_OK && pct >= 25 && pct <= 400) out->motion_speed_pct = pct;
    nvs_close(h);
}

int settings_save(const emo_settings_t *in)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    nvs_set_str(h, "wifi_ssid", in->wifi_ssid);
    nvs_set_str(h, "wifi_pass", in->wifi_pass);
    nvs_set_str(h, "host_ip", in->host_ip);
    nvs_set_u16(h, "host_port", in->host_port);
    nvs_set_u16(h, "motion_speed", in->motion_speed_pct);
    int rc = (nvs_commit(h) == ESP_OK) ? 0 : -1;
    nvs_close(h);
    if (rc == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_live = *in;
        xSemaphoreGive(s_lock);
    }
    return rc;
}

void settings_get_copy(emo_settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_live;
    xSemaphoreGive(s_lock);
}

// Bounded copy that always NUL-terminates.
static void cpy(char *dst, size_t cap, const char *src)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

int settings_set_kv(emo_settings_t *s, const char *k, const char *v)
{
    if (!strcmp(k, "wifi_ssid")) cpy(s->wifi_ssid, sizeof(s->wifi_ssid), v);
    else if (!strcmp(k, "wifi_pass")) cpy(s->wifi_pass, sizeof(s->wifi_pass), v);
    else if (!strcmp(k, "host_ip")) {
        struct in_addr a;
        if (inet_pton(AF_INET, v, &a) != 1) return -1;          // dotted IPv4 only
        cpy(s->host_ip, sizeof(s->host_ip), v);
    } else if (!strcmp(k, "host_port")) {
        long p = strtol(v, NULL, 10);
        if (p < 1 || p > 65535) return -1;
        s->host_port = (uint16_t)p;
    } else if (!strcmp(k, "motion_speed")) {
        long p = strtol(v, NULL, 10);
        if (p < 25 || p > 400) return -1;                      // percent of the file timing
        s->motion_speed_pct = (uint16_t)p;
    }
    else return -1;
    return 0;
}

// Minimal JSON string escaper into a fixed buffer.
static void json_field(char **p, char *end, const char *k, const char *v, bool redact)
{
    const char *shown = v;
    char red[8];
    if (redact && v[0]) { strcpy(red, "***set"); shown = red; }
    int n = snprintf(*p, end - *p, "\"%s\":\"", k);
    if (n > 0) *p += n;
    for (const char *c = shown; *c && *p < end - 2; c++) {
        if (*c == '"' || *c == '\\') *(*p)++ = '\\';
        *(*p)++ = *c;
    }
    n = snprintf(*p, end - *p, "\",");
    if (n > 0) *p += n;
}

char *settings_to_json(const emo_settings_t *s, bool redact)
{
    size_t cap = 320;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    char *p = buf, *end = buf + cap;
    *p++ = '{';
    json_field(&p, end, "wifi_ssid", s->wifi_ssid, false);
    json_field(&p, end, "wifi_pass", s->wifi_pass, redact);
    json_field(&p, end, "host_ip", s->host_ip, false);
    int n = snprintf(p, end - p, "\"host_port\":%u,\"motion_speed\":%u,", (unsigned)s->host_port, (unsigned)s->motion_speed_pct);
    if (n > 0) p += n;
    if (p > buf + 1 && p[-1] == ',') p--;          // drop the trailing comma
    if (p < end - 1) *p++ = '}';
    *p = '\0';
    return buf;
}

void settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    settings_load(&s_live);
}
