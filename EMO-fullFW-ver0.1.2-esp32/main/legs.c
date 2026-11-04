// legs.c — see legs.h.
#include "legs.h"
#include "esp_heap_caps.h"
#include "servo.h"
#include "settings.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "legs";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MOT_DIR       "/spiffs/mot"
#define MEDIA_TABLE   "/spiffs/mot_media.txt"   // "mot media mp3flag" per line (from profile.json)
#define FRAME_LEN     20
#define MODE_DIRECT   0xFE
#define MODE_SKIP     0xFD
#define TICK_MS       20                 // stock servo update period
#define MAX_FILE      65536
#define MAX_POSE_MS   10000

static SemaphoreHandle_t s_api;          // serialises play / pose / stop / release
static volatile bool s_stop;             // asks the play task to end
static volatile bool s_running;          // play task alive
static legs_status_t s_st;
static char *s_list_json;                // cached GET list
static legs_media_cb_t s_media_cb;
static int64_t s_t0_us;                  // playback start (esp_timer)
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_idle_timer;  // releases the servos after a move (EMO_SERVO_IDLE_MS)
static float s_default_speed = 1.0f;
static int64_t s_deadline_us;            // absolute schedule of the running playback (no drift)

void legs_set_default_speed(float speed)
{
    if (!(speed >= 0.25f && speed <= 4.0f)) speed = 1.0f;
    s_default_speed = speed;
    s_st.speed_default = speed;
}
float legs_default_speed(void) { return s_default_speed; }

// Sleep until the next 20 ms slot of the absolute schedule. Every tick is placed
// at start + k*20 ms, so the time spent writing the servos never accumulates and a
// 40 s dance ends at 40 s, not 40 s plus 2000 ticks of overhead.
static void tick_sleep(uint32_t ms)
{
    s_deadline_us += (int64_t)ms * 1000;
    int64_t wait = s_deadline_us - esp_timer_get_time();
    if (wait > 0) {
        vTaskDelay(pdMS_TO_TICKS((wait + 999) / 1000));
    } else {
        // Behind schedule (LEDC can burn CPU): still yield so IDLE feeds the WDT.
        if (wait < -200000) s_deadline_us = esp_timer_get_time();
        taskYIELD();
    }
}

void legs_set_media_cb(legs_media_cb_t cb) { s_media_cb = cb; }

static void publish_pos(void)
{
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < 4; i++) s_st.pos[i] = (uint8_t)(servo_pos(i) + 0.5f);
    s_st.powered = servo_powered();
    portEXIT_CRITICAL(&s_mux);
}

// ---- idle release: a holding servo draws current for nothing ----
static void idle_release(void *arg)
{
    xSemaphoreTake(s_api, portMAX_DELAY);
    if (!s_running && servo_powered()) {              // nothing started meanwhile
        servo_release();
        publish_pos();
        ESP_LOGI(TAG, "servos idle for %d ms -> released", CONFIG_EMO_SERVO_IDLE_MS);
    }
    xSemaphoreGive(s_api);
}

// (Re)start the countdown; called whenever a move ends. 0 = keep holding.
static void arm_idle_release(void)
{
    if (!s_idle_timer || CONFIG_EMO_SERVO_IDLE_MS <= 0) return;
    esp_timer_stop(s_idle_timer);
    esp_timer_start_once(s_idle_timer, (uint64_t)CONFIG_EMO_SERVO_IDLE_MS * 1000);
}

static void cancel_idle_release(void)
{
    if (s_idle_timer) esp_timer_stop(s_idle_timer);
}

// Linear ramp from the current pose to tgt over ms, in 20 ms ticks (stock
// FUN_4025a504). Returns false if interrupted by s_stop.
static bool move_to(const uint8_t tgt[4], uint32_t ms)
{
    servo_power_on();
    int steps = ms / TICK_MS;
    if (steps < 1) {
        for (int i = 0; i < 4; i++) servo_write(i, tgt[i]);
        servo_flush();
        publish_pos();
        if (ms) tick_sleep(ms);
        return !s_stop;
    }
    float start[4], inc[4];
    for (int i = 0; i < 4; i++) { start[i] = servo_pos(i); inc[i] = (tgt[i] - start[i]) / steps; }
    for (int k = 1; k <= steps; k++) {
        if (s_stop) return false;
        for (int i = 0; i < 4; i++) servo_write(i, k == steps ? tgt[i] : start[i] + inc[i] * k);
        servo_flush();
        publish_pos();
        tick_sleep(TICK_MS);
    }
    if (ms % TICK_MS) tick_sleep(ms % TICK_MS);          // keep the file's exact total
    return true;
}

static bool hold(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += TICK_MS) {
        if (s_stop) return false;
        tick_sleep(t + TICK_MS <= ms ? TICK_MS : ms - t);
    }
    return true;
}

// ---- procedural gait (stock oscillateServos / _execute) ----
#define SAMPLE_MS 20
#define GAIT_MIN_CYCLES 0.05f

static bool run_gait(const legs_gait_t *g)
{
    int samples = (int)(g->period_ms / SAMPLE_MS);          // stock: T / 20, integer
    if (samples < 1) samples = 1;
    double inc = 2.0 * M_PI / samples;
    double phase[4], phase0[4];
    for (int i = 0; i < 4; i++) {
        phase[i] = g->dir_deg / 360.0 * 2.0 * M_PI;           // stock FUN_4025a2e4
        phase0[i] = g->phase0[i] * M_PI / 180.0;
    }
    servo_power_on();
    float cycles = g->cycles < GAIT_MIN_CYCLES ? GAIT_MIN_CYCLES : g->cycles;
    int full = (int)cycles;
    float frac = cycles - full;
    // stock _execute: `full` whole periods, then the fractional remainder (x < frac*samples)
    int total = full * samples + (int)ceilf(frac * samples - 1e-4f);
    for (int k = 0; k < total; k++) {
        if (s_stop) return false;
        for (int i = 0; i < 4; i++) {
            int pos = (int)lround(g->A[i] * sin(phase[i] + phase0[i]) + g->O[i]);
            servo_write(i, 90.0f + pos);
            phase[i] += inc;
        }
        servo_flush();
        publish_pos();
        tick_sleep(SAMPLE_MS);
    }
    if (g->neutral_after) {
        static const uint8_t neutral[4] = { 90, 90, 90, 90 };
        return move_to(neutral, g->period_ms / 2);
    }
    return true;
}

// value > 200 encodes a negative amplitude (stock loader: 200 - v)
static int8_t amp(uint8_t v) { return (int8_t)(v > 200 ? 200 - (int)v : (int)v); }

// Gait frame (mode 1 = walk-type, mode 2 = turn-type) -> oscillator parameters,
// exactly as the stock motion_parse_file + FUN_4025c6b8 / FUN_4025d338 do.
static bool gait_from_frame(const uint8_t *f, legs_gait_t *g)
{
    uint8_t mode = f[5], count = f[6], foff = f[7], dir = f[8], famp = f[9], leg = f[10];
    uint32_t dur = f[16] | (uint32_t)f[17] << 8;
    memset(g, 0, sizeof(*g));
    g->cycles = count / 10.0f;
    g->period_ms = dur;
    g->dir_deg = dir;
    g->O[2] = (int8_t)foff; g->O[3] = (int8_t)-foff;
    g->neutral_after = (leg == 101 || leg == 102);
    if (mode == 0x01) {                                   // walk: hips and feet same amplitude
        g->A[0] = g->A[1] = amp(f[11]);
        g->A[2] = g->A[3] = amp(f[12]);
        float ph = (leg == 1 || leg == 101) ? -90.0f : (leg == 2 || leg == 102) ? 90.0f : 0.0f;
        g->phase0[2] = g->phase0[3] = ph;
        return true;
    }
    if (mode == 0x02) {                                   // turn: asymmetric hips, feet -90
        int8_t a = amp(f[11]), b = amp(f[12]);
        g->A[0] = 30; g->A[1] = 30;
        if (leg == 1 || leg == 101)      { g->A[0] = a; g->A[1] = b; }
        else if (leg == 2 || leg == 102) { g->A[0] = b; g->A[1] = a; }
        g->A[2] = g->A[3] = (int8_t)famp;
        g->phase0[2] = g->phase0[3] = -90.0f;
        return true;
    }
    return false;
}

void legs_gait_preset(bool turn, bool reverse, legs_gait_t *out)
{
    memset(out, 0, sizeof(*out));
    out->cycles = 1;
    if (!turn) {                                          // explore_foward1 / explore_back_away1
        if (!reverse) { out->period_ms = 400; out->A[0] = out->A[1] = 8;  out->A[2] = out->A[3] = 8;  out->phase0[2] = out->phase0[3] = -90; }
        else          { out->period_ms = 500; out->A[0] = out->A[1] = 10; out->A[2] = out->A[3] = 10; out->phase0[2] = out->phase0[3] = 90; }
        out->O[2] = 2; out->O[3] = -2;
    } else {                                              // explore_turn_left1 / explore_turn_right1
        out->period_ms = 600;
        out->A[0] = reverse ? -8 : 8; out->A[1] = reverse ? 8 : -8;
        out->A[2] = out->A[3] = 12;
        out->O[2] = 3; out->O[3] = -3;
        out->phase0[2] = out->phase0[3] = -90;
        out->dir_deg = reverse ? 180 : 0;
    }
}

static void gait_task(void *arg)
{
    legs_gait_t g = *(legs_gait_t *)arg;
    free(arg);
    s_t0_us = s_deadline_us = esp_timer_get_time();
    bool alive = run_gait(&g);
    s_st.elapsed_ms = (uint32_t)((esp_timer_get_time() - s_t0_us) / 1000);
    ESP_LOGI(TAG, "gait: %s after %u ms", alive ? "done" : "stopped", (unsigned)s_st.elapsed_ms);
    s_st.playing = false;
    s_running = false;
    arm_idle_release();
    vTaskDelete(NULL);
}

// ---- .mot frames ----
static bool frame_ok(const uint8_t *f)
{
    if (f[0] != 0x55 || f[1] != 0xAA || f[2] != 0x55 || f[3] != 0xAA) return false;
    if (f[4] != 0x02 && f[4] != 0x0E) return false;
    if (f[19] != 0xED) return false;
    uint32_t sum = 0;
    for (int i = 4; i <= 17; i++) sum += f[i];
    return (sum & 0xFF) == f[18];
}

typedef struct { uint8_t *buf; size_t len; bool loop; float speed; } play_arg_t;

static void play_task(void *arg)
{
    play_arg_t a = *(play_arg_t *)arg;
    free(arg);
    size_t nframes = a.len / FRAME_LEN;
    bool alive = true;
    s_t0_us = s_deadline_us = esp_timer_get_time();
    do {
        for (size_t i = 0; i < nframes && alive; i++) {
            const uint8_t *f = a.buf + i * FRAME_LEN;
            if (!frame_ok(f)) continue;
            uint32_t dur = (uint32_t)((f[16] | (uint32_t)f[17] << 8) / a.speed + 0.5f);
            s_st.frame = (uint16_t)(i + 1);
            s_st.elapsed_ms = (uint32_t)((esp_timer_get_time() - s_t0_us) / 1000);
            legs_gait_t g;
            switch (f[5]) {
            case MODE_DIRECT: alive = move_to(f + 11, dur); break;
            case MODE_SKIP:   break;
            default:
                if (gait_from_frame(f, &g)) {                // stock walk / turn oscillator
                    if (a.speed != 1.0f) g.period_ms = (uint32_t)(g.period_ms / a.speed + 0.5f);
                    alive = run_gait(&g);
                } else {
                    s_st.unsupported++; alive = hold(dur);   // other stock generators (dances)
                }
                break;
            }
        }
    } while (a.loop && alive);
    free(a.buf);
    s_st.elapsed_ms = (uint32_t)((esp_timer_get_time() - s_t0_us) / 1000);
    ESP_LOGI(TAG, "%s: %s after %u ms (file %u ms at x%.2f)", s_st.name, alive ? "done" : "stopped",
             (unsigned)s_st.elapsed_ms, (unsigned)s_st.expected_ms, s_st.speed);
    s_st.playing = false;
    s_running = false;
    arm_idle_release();
    vTaskDelete(NULL);
}

// Face/sound asset paired with a motion: mot_media.txt (generated from the stock
// profile.json: "mot media mp3flag"), else the motion's own name without mp3.
static void media_for(const char *mot, char *media, size_t n, bool *mp3, uint32_t *mp3_ms)
{
    strlcpy(media, mot, n);
    *mp3 = false;
    *mp3_ms = 0;
    FILE *fp = fopen(MEDIA_TABLE, "r");
    if (!fp) return;
    char line[96], a[40], b[40];
    int flag; unsigned ms = 0;
    while (fgets(line, sizeof(line), fp)) {
        int got = sscanf(line, "%39s %39s %d %u", a, b, &flag, &ms);
        if (got >= 3 && strcmp(a, mot) == 0) {
            strlcpy(media, b, n);
            *mp3 = flag != 0;
            *mp3_ms = got >= 4 ? ms : 0;
            break;
        }
    }
    fclose(fp);
}

// Wall time the file takes at speed 1: direct frames = duration, gait frames =
// count/10 cycles x period (+ period/2 back to neutral), other generators = held.
static uint32_t file_time_ms(const uint8_t *buf, size_t len)
{
    uint32_t t = 0;
    for (size_t i = 0; i + FRAME_LEN <= len; i += FRAME_LEN) {
        const uint8_t *f = buf + i;
        if (!frame_ok(f) || f[5] == MODE_SKIP) continue;
        uint32_t dur = f[16] | (uint32_t)f[17] << 8;
        if (f[5] == 0x01 || f[5] == 0x02) t += (uint32_t)(f[6] / 10.0f * dur + 0.5f) + ((f[10] == 101 || f[10] == 102) ? dur / 2 : 0);
        else t += dur;
    }
    return t;
}

static void stop_locked(void)
{
    if (!s_running) return;
    s_stop = true;
    for (int i = 0; i < 300 && s_running; i++) vTaskDelay(pdMS_TO_TICKS(10));
    s_stop = false;
}

static bool name_ok(const char *n)
{
    size_t len = strlen(n);
    if (!len || len > 35) return false;
    for (; *n; n++)
        if (!((*n >= 'a' && *n <= 'z') || (*n >= 'A' && *n <= 'Z') || (*n >= '0' && *n <= '9') ||
              *n == '_' || *n == '-' || *n == '.')) return false;
    return true;
}

esp_err_t legs_play(const char *name, legs_opts_t opts)
{
    if (!s_st.fs_ok) return ESP_ERR_INVALID_STATE;
    if (!name || !name_ok(name) || strstr(name, "..")) return ESP_ERR_INVALID_ARG;
    if (opts.speed == 0.0f) opts.speed = s_default_speed;
    if (!(opts.speed >= 0.25f && opts.speed <= 4.0f)) opts.speed = 1.0f;
    bool loop = opts.loop;
    char path[64];
    snprintf(path, sizeof(path), MOT_DIR "/%s.mot", name);
    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    if (st.st_size < FRAME_LEN || st.st_size > MAX_FILE) return ESP_ERR_INVALID_SIZE;

    uint8_t *buf = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(st.st_size);
    if (!buf) return ESP_ERR_NO_MEM;
    FILE *fp = fopen(path, "rb");
    if (!fp) { free(buf); return ESP_ERR_NOT_FOUND; }
    size_t len = fread(buf, 1, st.st_size, fp);
    fclose(fp);
    size_t good = 0;
    for (size_t i = 0; i + FRAME_LEN <= len; i += FRAME_LEN)
        if (frame_ok(buf + i)) good++;
    if (!good) { free(buf); return ESP_ERR_INVALID_SIZE; }
    uint32_t total_ms = file_time_ms(buf, len);

    char media[40] = ""; bool mp3 = false; uint32_t mp3_ms = 0;
    media_for(name, media, sizeof(media), &mp3, &mp3_ms);
    if (opts.fit_music) {                                 // end exactly with the song
        if (mp3 && mp3_ms > 500 && total_ms > 0) {
            opts.speed = (float)total_ms / (float)mp3_ms;
            if (opts.speed < 0.5f) opts.speed = 0.5f;
            if (opts.speed > 2.0f) opts.speed = 2.0f;
        }
    }

    xSemaphoreTake(s_api, portMAX_DELAY);
    stop_locked();
    cancel_idle_release();
    play_arg_t *a = malloc(sizeof(*a));
    if (!a) { xSemaphoreGive(s_api); free(buf); return ESP_ERR_NO_MEM; }
    *a = (play_arg_t){ .buf = buf, .len = len - len % FRAME_LEN, .loop = loop, .speed = opts.speed };
    strlcpy(s_st.name, name, sizeof(s_st.name));
    strlcpy(s_st.media, opts.media ? media : "", sizeof(s_st.media));
    s_st.loop = loop; s_st.frame = 0; s_st.frames = (uint16_t)(len / FRAME_LEN);
    s_st.unsupported = 0; s_st.plays++; s_st.playing = true;
    s_st.speed = opts.speed; s_st.expected_ms = (uint32_t)(total_ms / opts.speed); s_st.elapsed_ms = 0;
    s_st.music_ms = mp3 ? mp3_ms : 0;
    a->speed = opts.speed;
    s_running = true;
    if (xTaskCreate(play_task, "legs", 4096, a, 4, NULL) != pdPASS) {
        s_running = false; s_st.playing = false;
        free(a); free(buf);
        xSemaphoreGive(s_api);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_api);
    ESP_LOGI(TAG, "play %s (%u frames, %u valid, %u ms at x%.2f%s%s%s%s)", name, (unsigned)(len / FRAME_LEN),
             (unsigned)good, (unsigned)s_st.expected_ms, opts.speed, loop ? ", loop" : "",
             opts.fit_music ? ", fit to music" : "", opts.media ? ", media " : "", opts.media ? media : "");
    // Face + music after the legs are already running: the stock sends the media
    // first, but its ACK/ANIM_DONE waits (up to 0.7 s) would delay the first frame.
    if (opts.media && s_media_cb) s_media_cb(media, mp3);
    return ESP_OK;
}

void legs_stop(void)
{
    xSemaphoreTake(s_api, portMAX_DELAY);
    stop_locked();
    arm_idle_release();
    xSemaphoreGive(s_api);
}

esp_err_t legs_pose_hold(const uint8_t deg[4], uint32_t ms, bool hold)
{
    for (int i = 0; i < 4; i++) if (deg[i] > 180) return ESP_ERR_INVALID_ARG;
    if (ms > MAX_POSE_MS) ms = MAX_POSE_MS;
    xSemaphoreTake(s_api, portMAX_DELAY);
    stop_locked();
    cancel_idle_release();
    s_deadline_us = esp_timer_get_time();
    move_to(deg, ms);
    ESP_LOGI(TAG, "pose [%u %u %u %u] pulse=[%u %u %u %u]µs hold=%d",
             deg[0], deg[1], deg[2], deg[3],
             (unsigned)servo_pulse_us(0), (unsigned)servo_pulse_us(1),
             (unsigned)servo_pulse_us(2), (unsigned)servo_pulse_us(3), (int)hold);
    if (!hold) arm_idle_release();
    xSemaphoreGive(s_api);
    return ESP_OK;
}

esp_err_t legs_pose(const uint8_t deg[4], uint32_t ms)
{
    return legs_pose_hold(deg, ms, false);
}

esp_err_t legs_gait(const legs_gait_t *g)
{
    if (!g || !(g->cycles > 0) || g->cycles > 60 || g->period_ms < 100 || g->period_ms > 5000) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_api, portMAX_DELAY);
    stop_locked();
    cancel_idle_release();
    legs_gait_t *a = malloc(sizeof(*a));
    if (!a) { xSemaphoreGive(s_api); return ESP_ERR_NO_MEM; }
    *a = *g;
    snprintf(s_st.name, sizeof(s_st.name), "gait x%.1f", g->cycles);
    s_st.media[0] = '\0';
    s_st.loop = false; s_st.frame = 0; s_st.frames = 0; s_st.unsupported = 0;
    s_st.plays++; s_st.playing = true; s_st.speed = 1.0f;
    s_st.expected_ms = (uint32_t)(g->cycles * g->period_ms) + (g->neutral_after ? g->period_ms / 2 : 0);
    s_st.elapsed_ms = 0;
    s_running = true;
    if (xTaskCreate(gait_task, "gait", 4096, a, 4, NULL) != pdPASS) {
        s_running = false; s_st.playing = false; free(a);
        xSemaphoreGive(s_api);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_api);
    ESP_LOGI(TAG, "gait: %.2f cycles of %u ms, A=[%d %d %d %d] O=[%d %d %d %d] dir=%.0f", g->cycles,
             (unsigned)g->period_ms, g->A[0], g->A[1], g->A[2], g->A[3], g->O[0], g->O[1], g->O[2], g->O[3], g->dir_deg);
    return ESP_OK;
}

void legs_release(void)
{
    xSemaphoreTake(s_api, portMAX_DELAY);
    stop_locked();
    cancel_idle_release();
    servo_release();
    publish_pos();
    xSemaphoreGive(s_api);
    ESP_LOGI(TAG, "servos released");
}

void legs_get_status(legs_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
    size_t total = 0, used = 0;
    if (out->fs_ok && esp_spiffs_info("storage", &total, &used) == ESP_OK) {
        out->fs_total = total; out->fs_used = used;
    }
}

// ---- file list ----
static int cmp_name(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

static void build_list(void)
{
    char (*names)[40] = NULL;
    size_t n = 0, cap = 0;
    DIR *d = opendir(MOT_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            size_t len = strlen(e->d_name);
            if (len < 5 || len >= 40 || strcasecmp(e->d_name + len - 4, ".mot") != 0) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 128;
                void *p = realloc(names, cap * sizeof(*names));
                if (!p) break;
                names = p;
            }
            memcpy(names[n], e->d_name, len - 4);
            names[n][len - 4] = '\0';
            n++;
        }
        closedir(d);
    }
    if (names) qsort(names, n, sizeof(*names), cmp_name);
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
    free(names);
    free(s_list_json);
    s_list_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    s_st.files = (uint16_t)n;
}

const char *legs_list_json(void) { return s_list_json ? s_list_json : "[]"; }

esp_err_t legs_init(void)
{
    s_api = xSemaphoreCreateMutex();
    const esp_timer_create_args_t targs = { .callback = idle_release, .name = "servo_idle" };
    if (esp_timer_create(&targs, &s_idle_timer) != ESP_OK) s_idle_timer = NULL;
    publish_pos();
    emo_settings_t cfg; settings_get_copy(&cfg);
    legs_set_default_speed(cfg.motion_speed_pct / 100.0f);
    if (servo_powered()) arm_idle_release();         // EMO_SERVO_HOLD_AT_BOOT: hold, then release
    ESP_LOGI(TAG, "servos release %s, default motion speed x%.2f", CONFIG_EMO_SERVO_IDLE_MS > 0 ? "after idle" : "never (hold)", s_default_speed);

    esp_vfs_spiffs_conf_t fs = {
        .base_path = "/spiffs", .partition_label = "storage", .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t e = esp_vfs_spiffs_register(&fs);
    s_st.fs_ok = (e == ESP_OK);
    if (!s_st.fs_ok) {
        ESP_LOGW(TAG, "SPIFFS not mounted (%s): flash the spiffs image (idf.py flash builds it from spiffs_image/)", esp_err_to_name(e));
    } else {
        build_list();
        size_t total = 0, used = 0;
        esp_spiffs_info("storage", &total, &used);
        ESP_LOGI(TAG, "SPIFFS %u/%u KB, %u motions in " MOT_DIR, (unsigned)(used / 1024), (unsigned)(total / 1024), s_st.files);
    }
    return ESP_OK;
}
