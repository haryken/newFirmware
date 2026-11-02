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
    strncpy(s->voice_mode, "xiaozhi", sizeof(s->voice_mode) - 1);
    // Official Xiaozhi cloud OTA (same default as xiaozhi-esp32 Kconfig).
    strncpy(s->ota_url, "https://api.tenclass.net/xiaozhi/ota/", sizeof(s->ota_url) - 1);
    s->ws_url[0] = 0;
    s->xz_token[0] = 0;
    s->motion_speed_pct = CONFIG_EMO_MOTION_SPEED_PCT;
    s->docked = false;
    s->idle_faces = true;
    s->cliff_enable = true;
    s->petting_enable = true;
    s->yolo_react = true;
    s->explore_enable = true;
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
    load_str(h, "voice_mode", out->voice_mode, sizeof(out->voice_mode));
    load_str(h, "ota_url", out->ota_url, sizeof(out->ota_url));
    load_str(h, "ws_url", out->ws_url, sizeof(out->ws_url));
    load_str(h, "xz_token", out->xz_token, sizeof(out->xz_token));
    // Empty string in NVS used to wipe the default — restore cloud OTA URL.
    if (!out->ota_url[0]) {
        strncpy(out->ota_url, "https://api.tenclass.net/xiaozhi/ota/", sizeof(out->ota_url) - 1);
    }
    uint16_t port;
    if (nvs_get_u16(h, "host_port", &port) == ESP_OK && port) out->host_port = port;
    uint16_t pct;
    if (nvs_get_u16(h, "motion_speed", &pct) == ESP_OK && pct >= 25 && pct <= 400) out->motion_speed_pct = pct;
    uint8_t b;
    if (nvs_get_u8(h, "docked", &b) == ESP_OK) out->docked = b != 0;
    if (nvs_get_u8(h, "idle_faces", &b) == ESP_OK) out->idle_faces = b != 0;
    if (nvs_get_u8(h, "cliff_en", &b) == ESP_OK) out->cliff_enable = b != 0;
    if (nvs_get_u8(h, "pet_en", &b) == ESP_OK) out->petting_enable = b != 0;
    if (nvs_get_u8(h, "yolo_en", &b) == ESP_OK) out->yolo_react = b != 0;
    if (nvs_get_u8(h, "explore_en", &b) == ESP_OK) out->explore_enable = b != 0;
    // One-shot: clear stuck "docked" from earlier tests so auto-explore can start.
    uint8_t alive = 0;
    if (nvs_get_u8(h, "alive1", &alive) != ESP_OK || alive == 0) {
        out->docked = false;
        out->explore_enable = true;
        nvs_set_u8(h, "docked", 0);
        nvs_set_u8(h, "explore_en", 1);
        nvs_set_u8(h, "alive1", 1);
        nvs_commit(h);
        ESP_LOGI(TAG, "alive1 migrate: undock + explore_enable");
    }
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
    nvs_set_str(h, "voice_mode", in->voice_mode);
    nvs_set_str(h, "ota_url", in->ota_url);
    nvs_set_str(h, "ws_url", in->ws_url);
    nvs_set_str(h, "xz_token", in->xz_token);
    nvs_set_u16(h, "motion_speed", in->motion_speed_pct);
    nvs_set_u8(h, "docked", in->docked ? 1 : 0);
    nvs_set_u8(h, "idle_faces", in->idle_faces ? 1 : 0);
    nvs_set_u8(h, "cliff_en", in->cliff_enable ? 1 : 0);
    nvs_set_u8(h, "pet_en", in->petting_enable ? 1 : 0);
    nvs_set_u8(h, "yolo_en", in->yolo_react ? 1 : 0);
    nvs_set_u8(h, "explore_en", in->explore_enable ? 1 : 0);
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
    } else if (!strcmp(k, "voice_mode")) {
        if (strcmp(v, "emoa") && strcmp(v, "xiaozhi")) return -1;
        cpy(s->voice_mode, sizeof(s->voice_mode), v);
    } else if (!strcmp(k, "ota_url") || !strcmp(k, "ws_url")) {
        if (strlen(v) >= 160) return -1;
        // Empty clears the field. ota_url is http(s); ws_url is ws(s) or http(s).
        if (v[0]) {
            if (!strcmp(k, "ota_url")) {
                if (strncmp(v, "http://", 7) && strncmp(v, "https://", 8)) return -1;
            } else if (strncmp(v, "ws://", 5) && strncmp(v, "wss://", 6) &&
                       strncmp(v, "http://", 7) && strncmp(v, "https://", 8)) {
                return -1;
            }
        }
        if (!strcmp(k, "ota_url")) cpy(s->ota_url, sizeof(s->ota_url), v);
        else cpy(s->ws_url, sizeof(s->ws_url), v);
    } else if (!strcmp(k, "xz_token")) {
        if (strlen(v) >= sizeof(s->xz_token)) return -1;
        cpy(s->xz_token, sizeof(s->xz_token), v);
    } else if (!strcmp(k, "motion_speed")) {
        long p = strtol(v, NULL, 10);
        if (p < 25 || p > 400) return -1;                      // percent of the file timing
        s->motion_speed_pct = (uint16_t)p;
    } else if (!strcmp(k, "docked") || !strcmp(k, "idle_faces") || !strcmp(k, "cliff_enable") ||
               !strcmp(k, "petting_enable") || !strcmp(k, "yolo_react") || !strcmp(k, "explore_enable")) {
        bool on = !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "True") || !strcmp(v, "on");
        bool off = !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "False") || !strcmp(v, "off");
        if (!on && !off) return -1;
        if (!strcmp(k, "docked")) s->docked = on;
        else if (!strcmp(k, "idle_faces")) s->idle_faces = on;
        else if (!strcmp(k, "cliff_enable")) s->cliff_enable = on;
        else if (!strcmp(k, "petting_enable")) s->petting_enable = on;
        else if (!strcmp(k, "yolo_react")) s->yolo_react = on;
        else s->explore_enable = on;
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
    size_t cap = 1024;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    char *p = buf, *end = buf + cap;
    *p++ = '{';
    json_field(&p, end, "wifi_ssid", s->wifi_ssid, false);
    json_field(&p, end, "wifi_pass", s->wifi_pass, redact);
    json_field(&p, end, "host_ip", s->host_ip, false);
    json_field(&p, end, "voice_mode", s->voice_mode, false);
    json_field(&p, end, "ota_url", s->ota_url, false);
    json_field(&p, end, "ws_url", s->ws_url, false);
    json_field(&p, end, "xz_token", s->xz_token, redact);
    int n = snprintf(p, end - p,
        "\"host_port\":%u,\"motion_speed\":%u,"
        "\"docked\":%s,\"idle_faces\":%s,\"cliff_enable\":%s,\"petting_enable\":%s,"
        "\"yolo_react\":%s,\"explore_enable\":%s,",
        (unsigned)s->host_port, (unsigned)s->motion_speed_pct,
        s->docked ? "true" : "false", s->idle_faces ? "true" : "false",
        s->cliff_enable ? "true" : "false", s->petting_enable ? "true" : "false",
        s->yolo_react ? "true" : "false", s->explore_enable ? "true" : "false");
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
