// rest_api.c — see rest_api.h.
#include "rest_api.h"
#include "settings.h"
#include "net.h"
#include "k210_link.h"
#include "k210_controls.h"
#include "microphone.h"
#include "speaker.h"
#include "host_stream.h"
#include "battery.h"
#include "legs.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "api";

static char *read_body(httpd_req_t *r, int *out_len)
{
    int total = r->content_len;
    if (total <= 0 || total > 8192) {
        ESP_LOGW(TAG, "%s: body length %d not accepted", r->uri, total);
        return NULL;
    }
    char *buf = malloc(total + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(r, buf + got, total - got);
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[total] = '\0';
    if (out_len) *out_len = total;
    return buf;
}

// ---- /api/config ----
static esp_err_t get_config(httpd_req_t *r)
{
    emo_settings_t st; settings_get_copy(&st);
    char *json = settings_to_json(&st, true);     // redact secrets
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, json ? json : "{}");
    free(json);
    return ESP_OK;
}

static esp_err_t post_config(httpd_req_t *r)
{
    char *body = read_body(r, NULL);
    if (!body) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "json"); return ESP_FAIL; }

    emo_settings_t st; settings_get_copy(&st);
    cJSON *kv;
    const char *bad = NULL;
    cJSON_ArrayForEach(kv, j) {
        int rc = -1;
        if (cJSON_IsString(kv)) rc = settings_set_kv(&st, kv->string, kv->valuestring);
        else if (cJSON_IsNumber(kv)) {
            char num[16]; snprintf(num, sizeof(num), "%d", kv->valueint);
            rc = settings_set_kv(&st, kv->string, num);
        }
        if (rc != 0 && !bad) bad = kv->string;
    }
    httpd_resp_set_type(r, "application/json");
    if (bad) {
        char out[96];
        snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"invalid %s\"}", bad);
        cJSON_Delete(j);
        httpd_resp_set_status(r, "400 Bad Request");
        httpd_resp_sendstr(r, out);
        return ESP_OK;
    }
    cJSON_Delete(j);
    int rc = settings_save(&st);
    if (rc == 0) legs_set_default_speed(st.motion_speed_pct / 100.0f);   // applies to the next play
    httpd_resp_sendstr(r, rc == 0 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"nvs write failed\"}");
    return ESP_OK;
}

// ---- /api/status ----
static esp_err_t get_status(httpd_req_t *r)
{
    k210_stats_t k; k210_get_stats(&k);
    host_stream_stats_t a; host_stream_get_stats(&a);
    battery_t b; battery_get(&b);
    char ip[16]; net_get_ip(ip, sizeof(ip));
    char buf[640];
    snprintf(buf, sizeof(buf),
        "{\"ip\":\"%s\",\"linked\":%s,\"state\":%u,\"version\":\"%s\",\"boot_done\":%s,"
        "\"mic_open\":%s,\"mic_peak\":%u,\"mic_samples\":%u,\"play_queued\":%u,"
        "\"host_connected\":%s,\"fed\":%llu,\"acked\":%llu,\"backlog\":%u,"
        "\"overflow\":%u,\"reconnects\":%u,\"wake_events\":%u,\"session\":%s,\"sessions\":%u,"
        "\"bat_valid\":%s,\"bat_mv\":%u,\"bat_pct\":%u,\"bat_tier\":%u,\"bat_sense\":%s,\"bat_low\":%s}",
        ip, k.linked?"true":"false", (unsigned)k.state, k.version, k.boot_done?"true":"false",
        k.mic_open?"true":"false", (unsigned)k.mic_peak, (unsigned)k.mic_samples,
        (unsigned)k.play_queued, a.connected?"true":"false", (unsigned long long)a.fed,
        (unsigned long long)a.acked, (unsigned)a.backlog, (unsigned)a.overflow,
        (unsigned)a.reconnects, (unsigned)k.wake_events, a.session?"true":"false",
        (unsigned)a.sessions, b.valid?"true":"false", (unsigned)b.mv, (unsigned)b.percent,
        (unsigned)b.tier, b.sense?"true":"false", b.low?"true":"false");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, buf);
}

// ---- /api/tone, /api/play_pcm ----
static esp_err_t post_tone(httpd_req_t *r)
{
    speaker_tone(440, 500, 8000);
    httpd_resp_sendstr(r, "ok");
    return ESP_OK;
}

// Raw 48 kHz mono int16 LE body -> robot speaker. The body is fed into the play
// ring as it arrives; the handler holds the (single) HTTP task until the clip
// fits in the ring, and gives up when the clip is cut (barge-in) or the ring
// stops draining.
static esp_err_t post_play_pcm(httpd_req_t *r)
{
    if (!speaker_ready()) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no speaker (play ring not allocated)"); return ESP_FAIL; }
    uint8_t buf[2048];
    int remaining = r->content_len;
    uint32_t serial = speaker_serial();
    while (remaining > 0) {
        int n = httpd_req_recv(r, (char *)buf, sizeof(buf) < (size_t)remaining ? sizeof(buf) : remaining);
        if (n <= 0) { if (n == HTTPD_SOCK_ERR_TIMEOUT) continue; return ESP_FAIL; }
        size_t samples = n / 2, off = 0;
        int stalled_ms = 0;
        while (off < samples) {
            if (speaker_serial() != serial) {            // cut short: stop feeding, drop the rest
                httpd_resp_set_status(r, "409 Conflict");
                httpd_resp_sendstr(r, "interrupted");
                return ESP_FAIL;                         // closes the socket, unread body discarded
            }
            size_t d = speaker_play((const int16_t *)buf + off, samples - off);
            off += d;
            if (d) { stalled_ms = 0; continue; }
            if ((stalled_ms += 10) > 8000) {             // nothing drained for 8 s: the K210 is not playing
                speaker_interrupt("play ring not draining");
                httpd_resp_set_status(r, "503 Service Unavailable");
                httpd_resp_sendstr(r, "speaker not draining");
                return ESP_FAIL;
            }
            vTaskDelay(pdMS_TO_TICKS(10));               // ring full: let it drain
        }
        remaining -= n;
    }
    httpd_resp_sendstr(r, "ok");
    return ESP_OK;
}

// ---- /api/controls ----
static esp_err_t get_controls(httpd_req_t *r)
{
    char *json = k210_controls_list_json();
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, json ? json : "[]");
    free(json);
    return ESP_OK;
}

// {"action":"anim_stay_wink_1"} / {"action":"volume","value":80} / {"action":"screen_text","text":"hi"}
static esp_err_t post_controls(httpd_req_t *r)
{
    char *body = read_body(r, NULL);
    if (!body) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "json"); return ESP_FAIL; }
    char out[256];
    esp_err_t e = k210_controls_run(j, out, sizeof(out));
    cJSON_Delete(j);
    httpd_resp_set_type(r, "application/json");
    if (e == ESP_ERR_NOT_FOUND)          httpd_resp_set_status(r, "404 Not Found");
    else if (e == ESP_ERR_INVALID_STATE) httpd_resp_set_status(r, "409 Conflict");
    else if (e == ESP_ERR_NOT_SUPPORTED) httpd_resp_set_status(r, "403 Forbidden");
    else if (e != ESP_OK)                httpd_resp_set_status(r, "400 Bad Request");
    httpd_resp_sendstr(r, out);
    return ESP_OK;
}

// ---- /api/motion ----
static esp_err_t get_motion(httpd_req_t *r)
{
    legs_status_t m; legs_get_status(&m);
    char head[512];
    snprintf(head, sizeof(head),
        "{\"fs_ok\":%s,\"playing\":%s,\"loop\":%s,\"name\":\"%s\",\"media\":\"%s\",\"frame\":%u,\"frames\":%u,"
        "\"unsupported\":%u,\"pos\":[%u,%u,%u,%u],\"powered\":%s,\"plays\":%u,\"files\":%u,"
        "\"speed\":%.2f,\"speed_default\":%.2f,\"expected_ms\":%u,\"elapsed_ms\":%u,\"music_ms\":%u,"
        "\"fs_used\":%u,\"fs_total\":%u,\"list\":",
        m.fs_ok?"true":"false", m.playing?"true":"false", m.loop?"true":"false", m.name, m.media,
        (unsigned)m.frame, (unsigned)m.frames, (unsigned)m.unsupported,
        m.pos[0], m.pos[1], m.pos[2], m.pos[3], m.powered?"true":"false", (unsigned)m.plays,
        (unsigned)m.files, m.speed, m.speed_default, (unsigned)m.expected_ms, (unsigned)m.elapsed_ms, (unsigned)m.music_ms,
        (unsigned)m.fs_used, (unsigned)m.fs_total);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_send_chunk(r, head, strlen(head));
    const char *list = legs_list_json();
    httpd_resp_send_chunk(r, list, strlen(list));
    httpd_resp_send_chunk(r, "}", 1);
    httpd_resp_send_chunk(r, NULL, 0);
    return ESP_OK;
}

// {"action":"play","name":"look_around_1","loop":0,"media":true,"speed":1.0|"music"} | {"action":"stop"} |
// {"action":"pose","pos":[90,90,90,90],"ms":500} | {"action":"release"} |
// {"action":"gait","kind":"walk|turn","reverse":false,"cycles":2}
static esp_err_t post_motion(httpd_req_t *r)
{
    char *body = read_body(r, NULL);
    if (!body) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "json"); return ESP_FAIL; }
    const cJSON *act = cJSON_GetObjectItemCaseSensitive(j, "action");
    const char *a = cJSON_IsString(act) ? act->valuestring : "";
    esp_err_t e = ESP_OK;
    const char *err = NULL;
    if (!strcmp(a, "play")) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(j, "name");
        const cJSON *l = cJSON_GetObjectItemCaseSensitive(j, "loop");
        const cJSON *md = cJSON_GetObjectItemCaseSensitive(j, "media");
        const cJSON *sp = cJSON_GetObjectItemCaseSensitive(j, "speed");
        legs_opts_t o = LEGS_OPTS_DEFAULT;
        o.loop = (cJSON_IsNumber(l) && l->valueint) || cJSON_IsTrue(l);
        if (cJSON_IsFalse(md) || (cJSON_IsNumber(md) && !md->valueint)) o.media = false;
        if (cJSON_IsNumber(sp)) o.speed = (float)sp->valuedouble;             // absent/0 = default speed
        else if (cJSON_IsString(sp) && !strcmp(sp->valuestring, "music")) o.fit_music = true;
        e = legs_play(cJSON_IsString(n) ? n->valuestring : NULL, o);
        if (e == ESP_ERR_NOT_FOUND) err = "no such motion";
        else if (e == ESP_ERR_INVALID_ARG) err = "bad name";
        else if (e == ESP_ERR_INVALID_SIZE) err = "file has no valid frames";
        else if (e == ESP_ERR_INVALID_STATE) err = "SPIFFS not mounted";
        else if (e != ESP_OK) err = esp_err_to_name(e);
    } else if (!strcmp(a, "stop")) {
        legs_stop();
    } else if (!strcmp(a, "release")) {
        legs_release();
    } else if (!strcmp(a, "pose")) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(j, "pos");
        const cJSON *ms = cJSON_GetObjectItemCaseSensitive(j, "ms");
        uint8_t deg[4] = { 90, 90, 90, 90 };
        if (!cJSON_IsArray(p) || cJSON_GetArraySize(p) != 4) { e = ESP_ERR_INVALID_ARG; err = "pos must be 4 numbers 0..180"; }
        for (int i = 0; i < 4 && !err; i++) {
            const cJSON *v = cJSON_GetArrayItem(p, i);
            if (!cJSON_IsNumber(v) || v->valueint < 0 || v->valueint > 180) { e = ESP_ERR_INVALID_ARG; err = "pos must be 4 numbers 0..180"; }
            else deg[i] = (uint8_t)v->valueint;
        }
        if (!err) e = legs_pose(deg, cJSON_IsNumber(ms) ? (uint32_t)ms->valueint : 500);
    } else if (!strcmp(a, "gait")) {
        const cJSON *kind = cJSON_GetObjectItemCaseSensitive(j, "kind");
        const cJSON *rev = cJSON_GetObjectItemCaseSensitive(j, "reverse");
        const cJSON *cyc = cJSON_GetObjectItemCaseSensitive(j, "cycles");
        const cJSON *per = cJSON_GetObjectItemCaseSensitive(j, "period_ms");
        legs_gait_t g;
        bool turn = cJSON_IsString(kind) && !strcmp(kind->valuestring, "turn");
        if (!cJSON_IsString(kind) || (!turn && strcmp(kind->valuestring, "walk"))) { e = ESP_ERR_INVALID_ARG; err = "kind must be walk or turn"; }
        else {
            legs_gait_preset(turn, cJSON_IsTrue(rev) || (cJSON_IsNumber(rev) && rev->valueint), &g);
            if (cJSON_IsNumber(cyc)) g.cycles = (float)cyc->valuedouble;
            if (cJSON_IsNumber(per)) g.period_ms = (uint32_t)per->valueint;
            const char *keys[3] = { "A", "O", "phase0" };
            for (int k = 0; k < 3; k++) {                  // optional raw oscillator arrays
                const cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, keys[k]);
                if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 4)
                    for (int i = 0; i < 4; i++) {
                        double v = cJSON_GetArrayItem(arr, i)->valuedouble;
                        if (k == 0) g.A[i] = (int8_t)v; else if (k == 1) g.O[i] = (int8_t)v; else g.phase0[i] = (float)v;
                    }
            }
            const cJSON *dir = cJSON_GetObjectItemCaseSensitive(j, "dir");
            if (cJSON_IsNumber(dir)) g.dir_deg = (float)dir->valuedouble;
            e = legs_gait(&g);
            if (e == ESP_ERR_INVALID_ARG) err = "cycles 0..60, period_ms 100..5000";
        }
    } else {
        e = ESP_ERR_NOT_FOUND; err = "unknown action (play, gait, stop, pose, release)";
    }
    cJSON_Delete(j);
    char out[160];
    if (e == ESP_OK) snprintf(out, sizeof(out), "{\"ok\":true}");
    else snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"%s\"}", err ? err : esp_err_to_name(e));
    httpd_resp_set_type(r, "application/json");
    if (e == ESP_ERR_NOT_FOUND) httpd_resp_set_status(r, "404 Not Found");
    else if (e != ESP_OK)       httpd_resp_set_status(r, "400 Bad Request");
    httpd_resp_sendstr(r, out);
    return ESP_OK;
}

// ---- /api/power: {"action":"off"} drops the power-enable pin (whole robot off) ----
static void power_off_task(void *arg) { k210_link_power_off(); vTaskDelete(NULL); }

static esp_err_t post_power(httpd_req_t *r)
{
    char *body = read_body(r, NULL);
    if (!body) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "json"); return ESP_FAIL; }
    const cJSON *act = cJSON_GetObjectItemCaseSensitive(j, "action");
    bool off = cJSON_IsString(act) && !strcmp(act->valuestring, "off");
    cJSON_Delete(j);
    httpd_resp_set_type(r, "application/json");
    if (!off) { httpd_resp_set_status(r, "400 Bad Request"); httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"action must be off\"}"); return ESP_OK; }
    httpd_resp_sendstr(r, "{\"ok\":true}");           // reply first, then cut the power
    xTaskCreate(power_off_task, "poweroff", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ---- /api/mic.wav: 3 s capture through the host-stream monitor tap ----
#define CAP_SECONDS 3
static int16_t *volatile s_cap_buf;           // non-NULL while capturing
static volatile size_t s_cap_len, s_cap_want; // samples

static void on_mic(const int16_t *pcm, size_t samples, void *ctx)
{
    int16_t *cap = s_cap_buf;
    if (cap && s_cap_len < s_cap_want) {
        size_t n = s_cap_want - s_cap_len;
        if (n > samples) n = samples;
        memcpy(cap + s_cap_len, pcm, n * 2);
        s_cap_len += n;
    }
}

static esp_err_t get_mic_wav(httpd_req_t *r)
{
    size_t want = 16000 * CAP_SECONDS;
    int16_t *buf = heap_caps_malloc(want * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(want * 2);
    if (!buf) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    microphone_stats_t m; microphone_get_stats(&m);
    if (!m.open) microphone_open();
    s_cap_len = 0; s_cap_want = want; s_cap_buf = buf;
    for (int i = 0; i < CAP_SECONDS * 10 + 30 && s_cap_len < want; i++) vTaskDelay(pdMS_TO_TICKS(100));
    s_cap_buf = NULL;
    vTaskDelay(pdMS_TO_TICKS(50));            // let a tap in progress finish
    size_t n = s_cap_len, bytes = n * 2;
    uint8_t h[44];
    uint32_t u32; uint16_t u16;
    memcpy(h, "RIFF", 4); u32 = 36 + bytes; memcpy(h + 4, &u32, 4); memcpy(h + 8, "WAVEfmt ", 8);
    u32 = 16; memcpy(h + 16, &u32, 4); u16 = 1; memcpy(h + 20, &u16, 2); memcpy(h + 22, &u16, 2);
    u32 = 16000; memcpy(h + 24, &u32, 4); u32 = 32000; memcpy(h + 28, &u32, 4);
    u16 = 2; memcpy(h + 32, &u16, 2); u16 = 16; memcpy(h + 34, &u16, 2);
    memcpy(h + 36, "data", 4); u32 = bytes; memcpy(h + 40, &u32, 4);
    httpd_resp_set_type(r, "audio/wav");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_send_chunk(r, (const char *)h, sizeof(h));
    if (bytes) httpd_resp_send_chunk(r, (const char *)buf, bytes);
    httpd_resp_send_chunk(r, NULL, 0);
    free(buf);
    ESP_LOGI(TAG, "mic.wav: %u samples served", (unsigned)n);
    return ESP_OK;
}

void rest_api_register(httpd_handle_t srv)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/config",   .method = HTTP_GET,  .handler = get_config },
        { .uri = "/api/config",   .method = HTTP_POST, .handler = post_config },
        { .uri = "/api/status",   .method = HTTP_GET,  .handler = get_status },
        { .uri = "/api/tone",     .method = HTTP_POST, .handler = post_tone },
        { .uri = "/api/play_pcm", .method = HTTP_POST, .handler = post_play_pcm },
        { .uri = "/api/mic.wav",  .method = HTTP_GET,  .handler = get_mic_wav },
        { .uri = "/api/controls", .method = HTTP_GET,  .handler = get_controls },
        { .uri = "/api/controls", .method = HTTP_POST, .handler = post_controls },
        { .uri = "/api/motion",   .method = HTTP_GET,  .handler = get_motion },
        { .uri = "/api/motion",   .method = HTTP_POST, .handler = post_motion },
        { .uri = "/api/power",    .method = HTTP_POST, .handler = post_power },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(srv, &routes[i]);
        if (e != ESP_OK) ESP_LOGE(TAG, "register %s failed: %s", routes[i].uri, esp_err_to_name(e));
    }
    host_stream_set_monitor(on_mic, NULL);
}
