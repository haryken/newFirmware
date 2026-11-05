// daily.c - time-of-day routine scheduler (editable via /api/daily).
#include "daily.h"
#include "tricks.h"
#include "legs.h"
#include "behavior.h"
#include "k210_link.h"
#include "net.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "daily";

static const daily_slot_t k_default[] = {
    {  800,  815, "Boxing", "Dumbbell", 1 },
    {  815,  830, "shower", "", 1 },
    {  830,  840, "Brush_teeth", "", 1 },
    {  840,  850, "*", "", 1 },
    {  850,  900, "Eating", "", 1 },
    {  900,  920, "Newspaper", "", 1 },
    {  920,  930, "*", "", 1 },
    {  930, 1000, "Work", "Work_stamp", 1 },
    { 1000, 1015, "*", "", 1 },
    { 1100, 1130, "Cook", "", 1 },
    { 1130, 1140, "Eating", "", 1 },
    { 1140, 1200, "*", "", 1 },
    { 1400, 1415, "Work", "Work_stamp", 1 },
    { 1500, 1530, "Wipe_glass", "lightbulb", 1 },
    { 1530, 1545, "Teatime", "Blending", 1 },
    { 1615, 1630, "Juggling", "Magic_cube", 1 },
    { 1700, 1720, "Painting", "", 1 },
    { 1720, 1740, "meditation", "", 1 },
    { 1740, 1800, "Braid", "", 1 },
    { 1800, 1830, "Cook", "", 1 },
    { 1830, 1850, "Eating", "", 1 },
    { 1900, 2000, "Watch_tv", "", 1 },
    { 2000, 2030, "Read_a_book", "", 1 },
    { 2100, 2115, "hypnosis", "", 1 },
};
#define NDEF ((int)(sizeof(k_default) / sizeof(k_default[0])))

static const char *k_acts[] = {
    "Boxing", "Dumbbell", "Eating", "Newspaper", "Work", "Work_stamp", "Teatime", "Blending",
    "Painting", "Watch_tv", "Read_a_book", "Brush_teeth", "Braid", "Cook", "Wipe_glass",
    "lightbulb", "Juggling", "Magic_cube", "shower", "meditation", "hypnosis", "UVA", "poop",
    "banana", "aibi", "fried_chicken", "Eating2", "Eating3", "*", NULL
};
static const char *k_special[] = { "UVA", "poop", "banana", "aibi", "fried_chicken", "Eating2", "Eating3" };

static daily_slot_t s_slots[DAILY_MAX_SLOTS];
static int  s_n = 0;
static bool s_enabled = true;
static int  s_tz = 7;
static int  s_fake = -1;
static bool s_sntp;
static char s_last[28] = "";
static int  s_done_day = -1;
static uint8_t s_done[(DAILY_MAX_SLOTS + 7) / 8];

static void load_defaults(void)
{
    s_n = NDEF < DAILY_MAX_SLOTS ? NDEF : DAILY_MAX_SLOTS;
    memcpy(s_slots, k_default, s_n * sizeof(daily_slot_t));
}

static void nvs_load(void)
{
    load_defaults();
    nvs_handle_t h;
    if (nvs_open("emo_daily", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t en = 1; int8_t tz = 7;
    if (nvs_get_u8(h, "en", &en) == ESP_OK) s_enabled = en != 0;
    if (nvs_get_i8(h, "tz", &tz) == ESP_OK) s_tz = tz;
    size_t len = sizeof(s_slots);
    if (nvs_get_blob(h, "sched", s_slots, &len) == ESP_OK && len >= sizeof(daily_slot_t)) {
        s_n = (int)(len / sizeof(daily_slot_t));
        if (s_n > DAILY_MAX_SLOTS) s_n = DAILY_MAX_SLOTS;
    }
    nvs_close(h);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open("emo_daily", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "en", s_enabled ? 1 : 0);
    nvs_set_i8(h, "tz", (int8_t)s_tz);
    nvs_set_blob(h, "sched", s_slots, s_n * sizeof(daily_slot_t));
    nvs_commit(h);
    nvs_close(h);
}

static void apply_tz(void)
{
    char tz[24];
    snprintf(tz, sizeof(tz), "<%+03d>%d", s_tz, -s_tz);
    setenv("TZ", tz, 1);
    tzset();
}

static int now_hhmm(int *yday)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (yday) *yday = tm.tm_yday + 1000 * (tm.tm_year + 1900);
    if (s_fake >= 0) return s_fake;
    if (tm.tm_year + 1900 < 2024) return -1;
    return tm.tm_hour * 100 + tm.tm_min;
}

static bool idle_now(void)
{
    if (behavior_voice_busy()) return false;
    tricks_status_t ts; tricks_get_status(&ts);
    if (ts.busy) return false;
    legs_status_t lm; legs_get_status(&lm);
    if (lm.playing) return false;
    return k210_link_state() != LINK_SPEAK;
}

static void start_slot(int i)
{
    const daily_slot_t *s = &s_slots[i];
    const char *name = s->a;
    if (!strcmp(name, "*")) {
        if (esp_random() % 100 >= 50) return;
        name = k_special[esp_random() % (sizeof(k_special) / sizeof(k_special[0]))];
    } else if (s->b[0] && (esp_random() & 1)) {
        name = s->b;
    }
    char game[40];
    snprintf(game, sizeof(game), "daily:%s", name);
    esp_err_t e = tricks_game_start(game);
    ESP_LOGI(TAG, "slot %04d-%04d -> %s: %s", s->start, s->end, name, e == ESP_OK ? "started" : "busy");
    if (e == ESP_OK) snprintf(s_last, sizeof(s_last), "%04d %s", s->start, name);
}

static void daily_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    for (;;) {
        if (!s_sntp && net_is_connected()) {
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            s_sntp = true;
            ESP_LOGI(TAG, "SNTP started (tz UTC%+d)", s_tz);
        }
        int yday = 0;
        int hhmm = now_hhmm(&yday);
        if (yday != s_done_day) { s_done_day = yday; memset(s_done, 0, sizeof(s_done)); }
        if (s_enabled && hhmm >= 0 && esp_timer_get_time() > 60000000LL) {
            for (int i = 0; i < s_n; i++) {
                const daily_slot_t *s = &s_slots[i];
                if (!s->enabled) continue;
                if (hhmm < s->start || hhmm >= s->end) continue;
                if (s_done[i / 8] & (1 << (i % 8))) continue;
                if (!idle_now()) break;
                s_done[i / 8] |= (1 << (i % 8));
                start_slot(i);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void daily_init(void)
{
    nvs_load();
    apply_tz();
    xTaskCreate(daily_task, "daily", 4096, NULL, 3, NULL);
}

void daily_get_status(daily_status_t *o)
{
    memset(o, 0, sizeof(*o));
    o->enabled = s_enabled;
    o->fake_hhmm = s_fake;
    o->tz_hours = s_tz;
    o->nslots = s_n;
    memcpy(o->slots, s_slots, s_n * sizeof(daily_slot_t));
    int hhmm = now_hhmm(NULL);
    o->hhmm = hhmm;
    o->synced = hhmm >= 0 && s_fake < 0;
    strlcpy(o->last, s_last, sizeof(o->last));
    if (hhmm >= 0) {
        for (int i = 0; i < s_n; i++) {
            if (!s_slots[i].enabled) continue;
            if (s_slots[i].end > hhmm) {
                snprintf(o->next, sizeof(o->next), "%04d %s", s_slots[i].start, s_slots[i].a);
                break;
            }
        }
    }
}

void daily_set_enabled(bool on) { s_enabled = on; nvs_save(); }

void daily_set_fake_hhmm(int hhmm)
{
    s_fake = (hhmm >= 0 && hhmm < 2400 && hhmm % 100 < 60) ? hhmm : -1;
    s_done_day = -1;
}

esp_err_t daily_set_tz(int hours)
{
    if (hours < -12 || hours > 14) return ESP_ERR_INVALID_ARG;
    s_tz = hours;
    apply_tz();
    nvs_save();
    return ESP_OK;
}

esp_err_t daily_set_schedule(const daily_slot_t *slots, int n)
{
    if (!slots || n < 1 || n > DAILY_MAX_SLOTS) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < n; i++) {
        if (slots[i].start > 2359 || slots[i].end > 2400 || slots[i].start % 100 > 59) return ESP_ERR_INVALID_ARG;
        if (!slots[i].a[0]) return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_slots, slots, n * sizeof(daily_slot_t));
    s_n = n;
    s_done_day = -1;
    nvs_save();
    return ESP_OK;
}

void daily_reset_schedule(void)
{
    load_defaults();
    s_done_day = -1;
    nvs_save();
}

const char *const *daily_activity_names(void) { return (const char *const *)k_acts; }
