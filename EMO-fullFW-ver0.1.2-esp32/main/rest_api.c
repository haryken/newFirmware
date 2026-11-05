// rest_api.c — see rest_api.h.
#include "rest_api.h"
#include "settings.h"
#include "net.h"
#include "k210_link.h"
#include "k210_controls.h"
#include "microphone.h"
#include "speaker.h"
#include "host_stream.h"
#include "xiaozhi_client.h"
#include "battery.h"
#include "behavior.h"
#include "tricks.h"
#include "daily.h"
#include "k210_file.h"
#include "esp_heap_caps.h"
#include "legs.h"
#include "servo.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
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

    emo_settings_t before; settings_get_copy(&before);
    emo_settings_t st = before;
    cJSON *kv;
    const char *bad = NULL;
    bool wifi_changed = false;
    cJSON_ArrayForEach(kv, j) {
        int rc = -1;
        if (cJSON_IsString(kv)) rc = settings_set_kv(&st, kv->string, kv->valuestring);
        else if (cJSON_IsNumber(kv)) {
            char num[16]; snprintf(num, sizeof(num), "%d", kv->valueint);
            rc = settings_set_kv(&st, kv->string, num);
        } else if (cJSON_IsBool(kv)) {
            rc = settings_set_kv(&st, kv->string, cJSON_IsTrue(kv) ? "true" : "false");
        }
        if (rc != 0 && !bad) bad = kv->string;
        if (kv->string && (!strcmp(kv->string, "wifi_ssid") || !strcmp(kv->string, "wifi_pass")))
            wifi_changed = true;
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
    if (rc != 0) {
        httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"nvs write failed\"}");
        return ESP_OK;
    }
    if (wifi_changed && st.wifi_ssid[0]) {
        httpd_resp_sendstr(r, "{\"ok\":true,\"reboot\":true}");
        vTaskDelay(pdMS_TO_TICKS(400));
        esp_restart();
    }
    httpd_resp_sendstr(r, "{\"ok\":true}");
    return ESP_OK;
}

// ---- /api/wifi/scan ----
// Non-blocking: returns {"scanning":true} while a background scan runs, else AP list.
static esp_err_t get_wifi_scan(httpd_req_t *r)
{
    char *json = net_wifi_scan_json();
    httpd_resp_set_type(r, "application/json");
    if (!json) return httpd_resp_sendstr(r, "[]");
    esp_err_t e = httpd_resp_sendstr(r, json);
    free(json);
    return e;
}

// ---- GET /api/k210file?path=/spiffs/json/profile.json&max=200000 : raw bytes from the K210 storage ----
static esp_err_t get_k210file(httpd_req_t *r)
{
    char q[200] = "", path[160] = "", mx[12] = "";
    httpd_req_get_url_query_str(r, q, sizeof(q));
    if (httpd_query_key_value(q, "path", path, sizeof(path)) != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "path=");
        return ESP_OK;
    }
    // url-decode %2F etc. minimally
    char dec[160]; size_t j = 0;
    for (size_t i = 0; path[i] && j < sizeof(dec) - 1; i++) {
        if (path[i] == '%' && path[i + 1] && path[i + 2]) { char h[3] = { path[i + 1], path[i + 2], 0 }; dec[j++] = (char)strtol(h, NULL, 16); i += 2; }
        else dec[j++] = path[i];
    }
    dec[j] = 0;
    size_t cap = 200000;
    if (httpd_query_key_value(q, "max", mx, sizeof(mx)) == ESP_OK) { long v = atol(mx); if (v > 0 && v <= 1000000) cap = (size_t)v; }
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem"); return ESP_OK; }
    size_t n = 0;
    esp_err_t e = k210_file_read(dec, buf, cap, &n, 8000);
    if (e != ESP_OK) httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "no data");
    else { httpd_resp_set_type(r, "application/octet-stream"); httpd_resp_send(r, (const char *)buf, n); }
    free(buf);
    return ESP_OK;
}
// ---- /api/status ----
static esp_err_t get_status(httpd_req_t *r)
{
    k210_stats_t k; k210_get_stats(&k);
    host_stream_stats_t a; host_stream_get_stats(&a);
    xiaozhi_stats_t xz; xiaozhi_get_stats(&xz);
    emo_settings_t st; settings_get_copy(&st);
    battery_t b; battery_get(&b);
    behavior_status_t bh; behavior_get_status(&bh);
    char ip[16]; net_get_ip(ip, sizeof(ip));
    bool xz_mode = xz.enabled;
    bool session = xz_mode ? xz.session : a.session;
    unsigned sessions = xz_mode ? xz.sessions : a.sessions;
    bool host_up = xz_mode ? xz.connected : a.connected;
    char buf[1600];
    snprintf(buf, sizeof(buf),
        "{\"ip\":\"%s\",\"http_port\":%u,\"linked\":%s,\"state\":%u,\"version\":\"%s\",\"boot_done\":%s,"
        "\"mic_open\":%s,\"mic_peak\":%u,\"mic_samples\":%u,\"play_queued\":%u,"
        "\"voice_mode\":\"%s\",\"host_connected\":%s,\"fed\":%llu,\"acked\":%llu,\"backlog\":%u,"
        "\"overflow\":%u,\"reconnects\":%u,\"wake_events\":%u,\"session\":%s,\"sessions\":%u,"
        "\"xz_ready\":%s,\"xz_speaking\":%s,\"xz_listening\":%s,\"xz_activation\":\"%s\","
        "\"xz_device_id\":\"%s\",\"xz_client_id\":\"%s\",\"xz_ws_url\":\"%s\","
        "\"xz_opus_sent\":%u,\"xz_opus_recv\":%u,\"wifi_connected\":%s,"
        "\"bat_valid\":%s,\"bat_mv\":%u,\"bat_pct\":%u,\"bat_tier\":%u,\"bat_sense\":%s,\"bat_low\":%s,"
        "\"beh_mode\":\"%s\",\"docked\":%s,\"foot_ok\":%s,\"touch_ok\":%s,\"cliff_mask\":%u,"
        "\"cliff\":[%u,%u,%u,%u],\"petting\":%s}",
        ip, (unsigned)(net_is_softap() ? 80 : 8080),
        k.linked?"true":"false", (unsigned)k.state, k.version, k.boot_done?"true":"false",
        k.mic_open?"true":"false", (unsigned)k.mic_peak, (unsigned)k.mic_samples,
        (unsigned)k.play_queued, st.voice_mode, host_up?"true":"false",
        (unsigned long long)a.fed, (unsigned long long)a.acked, (unsigned)a.backlog,
        (unsigned)a.overflow, (unsigned)a.reconnects, (unsigned)k.wake_events,
        session?"true":"false", sessions,
        xz.ready?"true":"false", xz.speaking?"true":"false", xz.listening?"true":"false",
        xz.activation, xz.device_id, xz.client_id, xz.ws_url,
        (unsigned)xz.opus_sent, (unsigned)xz.opus_recv, net_is_connected()?"true":"false",
        b.valid?"true":"false", (unsigned)b.mv, (unsigned)b.percent,
        (unsigned)b.tier, b.sense?"true":"false", b.low?"true":"false",
        bh.mode, bh.docked?"true":"false", bh.foot_ok?"true":"false", bh.touch_ok?"true":"false",
        (unsigned)bh.cliff_mask, (unsigned)bh.cliff_ch[0], (unsigned)bh.cliff_ch[1],
        (unsigned)bh.cliff_ch[2], (unsigned)bh.cliff_ch[3], bh.petting?"true":"false");
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
    char head[704];
    snprintf(head, sizeof(head),
        "{\"fs_ok\":%s,\"playing\":%s,\"loop\":%s,\"name\":\"%s\",\"media\":\"%s\",\"frame\":%u,\"frames\":%u,"
        "\"unsupported\":%u,\"pos\":[%u,%u,%u,%u],\"pulse_us\":[%u,%u,%u,%u],\"gpio\":[%d,%d,%d,%d],"
        "\"board\":\"%s\",\"board_mode\":%d,\"pwr_gpio\":%d,"
        "\"rmt_cmd\":%u,\"powered\":%s,\"plays\":%u,\"files\":%u,"
        "\"ack_ok\":%d,\"ack_fail\":%d,\"diag\":\"%s\","
        "\"speed\":%.2f,\"speed_default\":%.2f,\"expected_ms\":%u,\"elapsed_ms\":%u,\"music_ms\":%u,"
        "\"fs_used\":%u,\"fs_total\":%u,\"list\":",
        m.fs_ok?"true":"false", m.playing?"true":"false", m.loop?"true":"false", m.name, m.media,
        (unsigned)m.frame, (unsigned)m.frames, (unsigned)m.unsupported,
        m.pos[0], m.pos[1], m.pos[2], m.pos[3],
        (unsigned)servo_pulse_us(0), (unsigned)servo_pulse_us(1),
        (unsigned)servo_pulse_us(2), (unsigned)servo_pulse_us(3),
        servo_gpio(0), servo_gpio(1), servo_gpio(2), servo_gpio(3),
        servo_board_name(), servo_board_mode(), servo_pwr_gpio(),
        (unsigned)servo_rmt_cmd(), m.powered?"true":"false", (unsigned)m.plays,
        (unsigned)m.files, servo_ack_ok(), servo_ack_fail(), servo_diag(),
        m.speed, m.speed_default, (unsigned)m.expected_ms, (unsigned)m.elapsed_ms, (unsigned)m.music_ms,
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
    } else if (!strcmp(a, "servo_board")) {
        // {"action":"servo_board","mode":"auto|old|new"} — saved in NVS, applied at the next boot.
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "mode");
        const char *ms = cJSON_IsString(m) ? m->valuestring : "";
        int mode = !strcmp(ms, "auto") ? 0 : !strcmp(ms, "old") ? 1 : !strcmp(ms, "new") ? 2 : -1;
        if (mode < 0) { e = ESP_ERR_INVALID_ARG; err = "mode must be auto|old|new"; }
        else e = servo_set_board_mode(mode);
    } else if (!strcmp(a, "rmt_cmd")) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(j, "cmd");
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "data");
        const cJSON *enc = cJSON_GetObjectItemCaseSensitive(j, "encode");
        uint8_t cmd = cJSON_IsNumber(c) ? (uint8_t)c->valueint : servo_rmt_cmd();
        uint16_t data = cJSON_IsNumber(d) ? (uint16_t)d->valueint : 0;
        if (cJSON_IsNumber(enc)) servo_set_encode(enc->valueint);
        servo_set_rmt_cmd(cmd);
        servo_rmt_poke(cmd, data);
    } else if (!strcmp(a, "bitbang")) {
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "deg");
        const cJSON *ms = cJSON_GetObjectItemCaseSensitive(j, "ms");
        int deg = cJSON_IsNumber(d) ? d->valueint : 90;
        int ms_v = cJSON_IsNumber(ms) ? ms->valueint : 2000;
        if (ms_v < 200) ms_v = 200;
        if (ms_v > 10000) ms_v = 10000;
        servo_bitbang_test(deg, ms_v);
    } else if (!strcmp(a, "pin_buzz")) {
        const cJSON *ms = cJSON_GetObjectItemCaseSensitive(j, "ms");
        int ms_v = cJSON_IsNumber(ms) ? ms->valueint : 5000;
        if (ms_v < 500) ms_v = 500;
        if (ms_v > 20000) ms_v = 20000;
        servo_pin_buzz(ms_v);
    } else if (!strcmp(a, "pose")) {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(j, "pos");
        const cJSON *ms = cJSON_GetObjectItemCaseSensitive(j, "ms");
        const cJSON *hold = cJSON_GetObjectItemCaseSensitive(j, "hold");
        uint8_t deg[4] = { 90, 90, 90, 90 };
        if (!cJSON_IsArray(p) || cJSON_GetArraySize(p) != 4) { e = ESP_ERR_INVALID_ARG; err = "pos must be 4 numbers 0..180"; }
        for (int i = 0; i < 4 && !err; i++) {
            const cJSON *v = cJSON_GetArrayItem(p, i);
            if (!cJSON_IsNumber(v) || v->valueint < 0 || v->valueint > 180) { e = ESP_ERR_INVALID_ARG; err = "pos must be 4 numbers 0..180"; }
            else deg[i] = (uint8_t)v->valueint;
        }
        bool keep = cJSON_IsTrue(hold) || (cJSON_IsNumber(hold) && hold->valueint);
        if (!err) e = legs_pose_hold(deg, cJSON_IsNumber(ms) ? (uint32_t)ms->valueint : 500, keep);
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
        e = ESP_ERR_NOT_FOUND; err = "unknown action (play, gait, stop, pose, release, bitbang, pin_buzz)";
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

static esp_err_t get_tricks(httpd_req_t *r)
{
    tricks_status_t t; tricks_get_status(&t);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"mode\":\"%s\",\"detail\":\"%s\",\"busy\":%s,\"timer_ms_left\":%u,\"explore_ms_left\":%u}",
             t.mode, t.detail, t.busy ? "true" : "false",
             (unsigned)t.timer_ms_left, (unsigned)t.explore_ms_left);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, buf);
}

static esp_err_t post_tricks(httpd_req_t *r)
{
    int len = 0;
    char *body = read_body(r, &len);
    if (!body) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "json"); return ESP_FAIL; }
    cJSON *action = cJSON_GetObjectItem(j, "action");
    esp_err_t e = ESP_ERR_INVALID_ARG;
    const char *err = "bad action";
    if (cJSON_IsString(action)) {
        const char *a = action->valuestring;
        if (!strcmp(a, "camping")) { e = tricks_camping_start(); err = "camping"; }
        else if (!strcmp(a, "timer")) {
            cJSON *sec = cJSON_GetObjectItem(j, "seconds");
            double s = cJSON_IsNumber(sec) ? sec->valuedouble : 0;
            e = tricks_timer_start((uint32_t)(s * 1000.0));
            err = "timer";
        } else if (!strcmp(a, "timer_cancel")) { tricks_timer_cancel(); e = ESP_OK; }
        else if (!strcmp(a, "explore")) {
            cJSON *sec = cJSON_GetObjectItem(j, "seconds");
            uint32_t s = cJSON_IsNumber(sec) ? (uint32_t)sec->valuedouble : 20;
            e = tricks_explore_start(s);
            err = (e == ESP_ERR_NOT_SUPPORTED) ? "cliff required" :
                  (e == ESP_ERR_INVALID_STATE) ? "busy or docked" : "explore";
        } else if (!strcmp(a, "explore_stop")) { tricks_explore_stop(); e = ESP_OK; }
        else if (!strcmp(a, "game")) {
            cJSON *g = cJSON_GetObjectItem(j, "game");
            e = cJSON_IsString(g) ? tricks_game_start(g->valuestring) : ESP_ERR_INVALID_ARG;
            err = "game (rps|ttt|zombie|fix_bugs)";
        } else if (!strcmp(a, "stop")) { tricks_stop(); e = ESP_OK; }
    }
    cJSON_Delete(j);
    if (e != ESP_OK) {
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", err);
        httpd_resp_set_type(r, "application/json");
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, buf);
    }
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"ok\":true}");
}

// ---- /api/daily: schedule + activity catalog ----
static esp_err_t get_daily(httpd_req_t *r)
{
    daily_status_t d; daily_get_status(&d);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", d.enabled);
    cJSON_AddBoolToObject(root, "synced", d.synced);
    cJSON_AddNumberToObject(root, "hhmm", d.hhmm);
    cJSON_AddNumberToObject(root, "fake", d.fake_hhmm);
    cJSON_AddNumberToObject(root, "tz", d.tz_hours);
    cJSON_AddStringToObject(root, "last", d.last);
    cJSON_AddStringToObject(root, "next", d.next);
    cJSON_AddBoolToObject(root, "legs_ok", legs_motion_allowed());
    cJSON *arr = cJSON_AddArrayToObject(root, "slots");
    for (int i = 0; i < d.nslots; i++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddNumberToObject(s, "start", d.slots[i].start);
        cJSON_AddNumberToObject(s, "end", d.slots[i].end);
        cJSON_AddStringToObject(s, "a", d.slots[i].a);
        cJSON_AddStringToObject(s, "b", d.slots[i].b);
        cJSON_AddBoolToObject(s, "enabled", d.slots[i].enabled != 0);
        cJSON_AddItemToArray(arr, s);
    }
    cJSON *acts = cJSON_AddArrayToObject(root, "activities");
    for (const char *const *p = daily_activity_names(); *p; p++) cJSON_AddItemToArray(acts, cJSON_CreateString(*p));
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_sendstr(r, out);
    free(out);
    return e;
}

static esp_err_t post_daily(httpd_req_t *r)
{
    char *body = read_body(r, NULL);
    if (!body) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "json"); return ESP_FAIL; }
    cJSON *act = cJSON_GetObjectItem(j, "action");
    if (cJSON_IsString(act) && !strcmp(act->valuestring, "reset")) daily_reset_schedule();
    cJSON *en = cJSON_GetObjectItem(j, "enable");
    cJSON *tz = cJSON_GetObjectItem(j, "tz");
    cJSON *fk = cJSON_GetObjectItem(j, "fake");
    if (cJSON_IsBool(en)) daily_set_enabled(cJSON_IsTrue(en));
    if (cJSON_IsNumber(tz)) daily_set_tz((int)tz->valuedouble);
    if (cJSON_IsNumber(fk)) daily_set_fake_hhmm((int)fk->valuedouble);
    cJSON *slots = cJSON_GetObjectItem(j, "slots");
    if (cJSON_IsArray(slots)) {
        daily_slot_t tmp[DAILY_MAX_SLOTS];
        int n = cJSON_GetArraySize(slots);
        if (n > DAILY_MAX_SLOTS) n = DAILY_MAX_SLOTS;
        memset(tmp, 0, sizeof(tmp));
        for (int i = 0; i < n; i++) {
            cJSON *s = cJSON_GetArrayItem(slots, i);
            cJSON *st = cJSON_GetObjectItem(s, "start");
            cJSON *en2 = cJSON_GetObjectItem(s, "end");
            cJSON *a = cJSON_GetObjectItem(s, "a");
            cJSON *b = cJSON_GetObjectItem(s, "b");
            cJSON *on = cJSON_GetObjectItem(s, "enabled");
            if (cJSON_IsNumber(st)) tmp[i].start = (uint16_t)st->valuedouble;
            if (cJSON_IsNumber(en2)) tmp[i].end = (uint16_t)en2->valuedouble;
            if (cJSON_IsString(a)) strlcpy(tmp[i].a, a->valuestring, sizeof(tmp[i].a));
            if (cJSON_IsString(b)) strlcpy(tmp[i].b, b->valuestring, sizeof(tmp[i].b));
            tmp[i].enabled = cJSON_IsFalse(on) ? 0 : 1;
        }
        if (daily_set_schedule(tmp, n) != ESP_OK) {
            cJSON_Delete(j);
            httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad schedule");
            return ESP_FAIL;
        }
    }
    cJSON *play = cJSON_GetObjectItem(j, "play");
    if (cJSON_IsString(play) && play->valuestring[0]) {
        char g[40];
        snprintf(g, sizeof(g), "daily:%s", play->valuestring);
        tricks_game_start(g);
    }
    cJSON_Delete(j);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"ok\":true}");
}

// ---- /api/xiaozhi: GET pair info; POST {"action":"pair"} refreshes OTA activation ----
static esp_err_t get_xiaozhi(httpd_req_t *r)
{
    xiaozhi_stats_t xz; xiaozhi_get_stats(&xz);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"enabled\":%s,\"wifi_connected\":%s,"
             "\"device_id\":\"%s\",\"client_id\":\"%s\","
             "\"activation\":\"%s\",\"ws_url\":\"%s\",\"ready\":%s,\"connected\":%s}",
             xz.enabled ? "true" : "false", net_is_connected() ? "true" : "false",
             xz.device_id, xz.client_id, xz.activation, xz.ws_url,
             xz.ready ? "true" : "false", xz.connected ? "true" : "false");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, buf);
}

static esp_err_t post_xiaozhi(httpd_req_t *r)
{
    char *body = read_body(r, NULL);
    if (!body) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "body"); return ESP_FAIL; }
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "json"); return ESP_FAIL; }
    const cJSON *act = cJSON_GetObjectItemCaseSensitive(j, "action");
    const char *a = cJSON_IsString(act) ? act->valuestring : "";
    bool pair = !strcmp(a, "pair") || !strcmp(a, "refresh");
    bool dismiss = !strcmp(a, "dismiss") || !strcmp(a, "clear");
    bool listen = !strcmp(a, "listen") || !strcmp(a, "wake");
    bool stop = !strcmp(a, "stop") || !strcmp(a, "end");
    cJSON_Delete(j);

    httpd_resp_set_type(r, "application/json");
    if (dismiss) {
        xiaozhi_dismiss_activation();
        return httpd_resp_sendstr(r, "{\"ok\":true,\"activation\":\"\"}");
    }
    if (listen) {
        if (!xiaozhi_enabled()) {
            httpd_resp_set_status(r, "409 Conflict");
            return httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"voice_mode not xiaozhi\"}");
        }
        // session_begin recovers K210 + opens mic (don't open mic before recover).
        xiaozhi_session_begin();
        return httpd_resp_sendstr(r, "{\"ok\":true,\"action\":\"listen\"}");
    }
    if (stop) {
        xiaozhi_session_end();
        return httpd_resp_sendstr(r, "{\"ok\":true,\"action\":\"stop\"}");
    }
    if (!pair) {
        httpd_resp_set_status(r, "400 Bad Request");
        return httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"action must be pair|dismiss|listen|stop\"}");
    }
    if (!xiaozhi_enabled()) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"set voice_mode=xiaozhi and Save first\"}");
    }
    esp_err_t e = xiaozhi_pair_refresh();
    xiaozhi_stats_t xz; xiaozhi_get_stats(&xz);
    char out[640];
    if (e == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(r, "409 Conflict");
        snprintf(out, sizeof(out),
                 "{\"ok\":false,\"error\":\"Wi-Fi STA not connected — join home Wi-Fi first\","
                 "\"device_id\":\"%s\",\"client_id\":\"%s\"}", xz.device_id, xz.client_id);
        return httpd_resp_sendstr(r, out);
    }
    if (e == ESP_ERR_INVALID_ARG) {
        httpd_resp_set_status(r, "400 Bad Request");
        return httpd_resp_sendstr(r, "{\"ok\":false,\"error\":\"ota_url empty\"}");
    }
    if (e != ESP_OK) {
        httpd_resp_set_status(r, "502 Bad Gateway");
        snprintf(out, sizeof(out),
                 "{\"ok\":false,\"error\":\"OTA failed: %s\",\"device_id\":\"%s\",\"client_id\":\"%s\"}",
                 esp_err_to_name(e), xz.device_id, xz.client_id);
        return httpd_resp_sendstr(r, out);
    }
    snprintf(out, sizeof(out),
             "{\"ok\":true,\"device_id\":\"%s\",\"client_id\":\"%s\","
             "\"activation\":\"%s\",\"ws_url\":\"%s\",\"ready\":%s}",
             xz.device_id, xz.client_id, xz.activation, xz.ws_url,
             xz.ready ? "true" : "false");
    return httpd_resp_sendstr(r, out);
}

void rest_api_register(httpd_handle_t srv)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/api/config",   .method = HTTP_GET,  .handler = get_config },
        { .uri = "/api/config",   .method = HTTP_POST, .handler = post_config },
        { .uri = "/api/wifi/scan",.method = HTTP_GET,  .handler = get_wifi_scan },
        { .uri = "/api/status",   .method = HTTP_GET,  .handler = get_status },
        { .uri = "/api/k210file", .method = HTTP_GET,  .handler = get_k210file },
        { .uri = "/api/xiaozhi",  .method = HTTP_GET,  .handler = get_xiaozhi },
        { .uri = "/api/xiaozhi",  .method = HTTP_POST, .handler = post_xiaozhi },
        { .uri = "/api/tone",     .method = HTTP_POST, .handler = post_tone },
        { .uri = "/api/play_pcm", .method = HTTP_POST, .handler = post_play_pcm },
        { .uri = "/api/mic.wav",  .method = HTTP_GET,  .handler = get_mic_wav },
        { .uri = "/api/controls", .method = HTTP_GET,  .handler = get_controls },
        { .uri = "/api/controls", .method = HTTP_POST, .handler = post_controls },
        { .uri = "/api/motion",   .method = HTTP_GET,  .handler = get_motion },
        { .uri = "/api/motion",   .method = HTTP_POST, .handler = post_motion },
        { .uri = "/api/tricks",   .method = HTTP_GET,  .handler = get_tricks },
        { .uri = "/api/tricks",   .method = HTTP_POST, .handler = post_tricks },
        { .uri = "/api/daily",    .method = HTTP_GET,  .handler = get_daily },
        { .uri = "/api/daily",    .method = HTTP_POST, .handler = post_daily },
        { .uri = "/api/power",    .method = HTTP_POST, .handler = post_power },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(srv, &routes[i]);
        if (e != ESP_OK) ESP_LOGE(TAG, "register %s failed: %s", routes[i].uri, esp_err_to_name(e));
    }
    host_stream_set_monitor(on_mic, NULL);
}
