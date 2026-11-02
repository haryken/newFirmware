// tricks.c — see tricks.h.
#include "tricks.h"
#include "behavior.h"
#include "foot_cliff.h"
#include "legs.h"
#include "video.h"
#include "k210_boot.h"
#include "k210_link.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "tricks";

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile bool s_stop;
static tricks_status_t s_st;
static esp_timer_handle_t s_timer;
static int64_t s_explore_deadline_us;
static int64_t s_timer_deadline_us;

static void set_st(const char *mode, const char *detail, bool busy)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_st.mode, mode ? mode : "idle", sizeof(s_st.mode));
    strlcpy(s_st.detail, detail ? detail : "", sizeof(s_st.detail));
    s_st.busy = busy;
    xSemaphoreGive(s_lock);
}

static void play_mot(const char *name)
{
    if (!name || !name[0]) return;
    legs_opts_t o = LEGS_OPTS_DEFAULT;
    o.media = true;
    if (legs_play(name, o) != ESP_OK) {
        bool ack = false;
        video_play_face(name, 0, true, &ack);
    }
}

static void wait_motion_or_stop(void)
{
    for (;;) {
        if (s_stop) return;
        legs_status_t m; legs_get_status(&m);
        if (!m.playing) return;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void wait_ms(uint32_t ms)
{
    while (ms && !s_stop) {
        uint32_t step = ms > 100 ? 100 : ms;
        vTaskDelay(pdMS_TO_TICKS(step));
        ms -= step;
    }
}

static void camping_body(void)
{
    static const char *steps[] = {
        "Camp_tent",
        "Camp_barbecue_start", "Camp_barbecue_loop1", "Camp_barbecue_end",
        "campfire_start", "campfire_loop1", "campfire_end",
    };
    set_st("camping", "running", true);
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]) && !s_stop; i++) {
        char d[40];
        snprintf(d, sizeof(d), "%s", steps[i]);
        set_st("camping", d, true);
        play_mot(steps[i]);
        wait_motion_or_stop();
        wait_ms(400);
    }
}

static void explore_body(uint32_t seconds)
{
    set_st("explore", "walking", true);
    s_explore_deadline_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    while (!s_stop && esp_timer_get_time() < s_explore_deadline_us) {
        if (behavior_docked()) break;
        // Prefer short forward / turn mots; cliff interrupt is in behavior.c.
        const char *step = (esp_random() % 3 == 0) ? "explore_turn_left1"
                         : (esp_random() % 2 == 0) ? "explore_foward1"
                                                   : "explore_turn_right1";
        play_mot(step);
        wait_motion_or_stop();
        if (s_stop) break;
        // brief look
        if ((esp_random() & 3) == 0) {
            play_mot("look_around_1");
            wait_motion_or_stop();
        }
        wait_ms(200);
    }
    legs_stop();
}

static void game_rps(void)
{
    set_st("game", "rps", true);
    play_mot("R_P_S_1");
    wait_motion_or_stop();
    if (s_stop) return;
    static const char *ends[] = { "R_P_S_win", "R_P_S_lose", "R_P_S_draw", "R_P_S_emo_victory" };
    play_mot(ends[esp_random() % 4]);
    wait_motion_or_stop();
}

static void game_ttt(void)
{
    set_st("game", "ttt", true);
    play_mot("ttt_i_will_play");
    wait_motion_or_stop();
    if (s_stop) return;
    play_mot("ttt_my_turn");
    wait_motion_or_stop();
    if (s_stop) return;
    play_mot((esp_random() & 1) ? "tictactoe_win1" : "tictactoe_lost1");
    wait_motion_or_stop();
}

static void game_zombie(void)
{
    set_st("game", "zombie", true);
    play_mot("zombie_start1");
    wait_motion_or_stop();
    if (s_stop) return;
    play_mot("zombie_loop_walk1");
    wait_motion_or_stop();
    if (s_stop) return;
    play_mot("zombie_shoot1");
    wait_motion_or_stop();
    if (s_stop) return;
    play_mot("zombie_shoot1_end");
    wait_motion_or_stop();
}

static void game_fix_bugs(void)
{
    set_st("game", "fix_bugs", true);
    play_mot("BUG_shooting");
    wait_motion_or_stop();
}

typedef struct {
    enum { JOB_CAMP, JOB_EXPLORE, JOB_GAME } kind;
    uint32_t seconds;
    char game[16];
} job_t;

static job_t s_job;

static void tricks_task(void *arg)
{
    (void)arg;
    job_t job = s_job;
    s_stop = false;
    switch (job.kind) {
    case JOB_CAMP: camping_body(); break;
    case JOB_EXPLORE: explore_body(job.seconds); break;
    case JOB_GAME:
        if (!strcmp(job.game, "rps")) game_rps();
        else if (!strcmp(job.game, "ttt")) game_ttt();
        else if (!strcmp(job.game, "zombie")) game_zombie();
        else if (!strcmp(job.game, "fix_bugs")) game_fix_bugs();
        break;
    }
    s_task = NULL;
    set_st("idle", "", false);
    s_explore_deadline_us = 0;
    vTaskDelete(NULL);
}

static esp_err_t launch(job_t job)
{
    if (!k210_boot_done()) return ESP_ERR_INVALID_STATE;
    if (behavior_docked() && job.kind == JOB_EXPLORE) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_task) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_job = job;
    s_stop = false;
    if (xTaskCreate(tricks_task, "tricks", 6144, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static void on_timer(void *arg)
{
    (void)arg;
    s_timer_deadline_us = 0;
    ESP_LOGI(TAG, "timer fired → alarm");
    set_st("timer", "alarm", true);
    play_mot("alarm_shock_start_1");
    // short ring; end clip after a beat via another one-shot-ish delay in task would be nicer,
    // but keep ISR light — schedule end on a tiny task.
}

void tricks_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(&s_st, 0, sizeof(s_st));
    strlcpy(s_st.mode, "idle", sizeof(s_st.mode));
    const esp_timer_create_args_t ta = {
        .callback = on_timer, .name = "emo_timer",
    };
    esp_timer_create(&ta, &s_timer);
}

void tricks_get_status(tricks_status_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_lock);
    int64_t now = esp_timer_get_time();
    if (s_timer_deadline_us > now)
        out->timer_ms_left = (uint32_t)((s_timer_deadline_us - now) / 1000);
    else out->timer_ms_left = 0;
    if (s_explore_deadline_us > now)
        out->explore_ms_left = (uint32_t)((s_explore_deadline_us - now) / 1000);
    else out->explore_ms_left = 0;
}

void tricks_stop(void)
{
    s_stop = true;
    tricks_timer_cancel();
    legs_stop();
    // wait briefly for task to exit
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    set_st("idle", "", false);
}

esp_err_t tricks_camping_start(void)
{
    job_t j = { .kind = JOB_CAMP };
    return launch(j);
}

esp_err_t tricks_timer_start(uint32_t ms)
{
    if (ms < 1000) ms = 1000;
    if (ms > 30 * 60 * 1000) ms = 30 * 60 * 1000;
    if (!s_timer) return ESP_ERR_INVALID_STATE;
    esp_timer_stop(s_timer);
    s_timer_deadline_us = esp_timer_get_time() + (int64_t)ms * 1000;
    esp_err_t e = esp_timer_start_once(s_timer, (uint64_t)ms * 1000);
    if (e == ESP_OK) {
        char d[40];
        snprintf(d, sizeof(d), "%u ms", (unsigned)ms);
        set_st("timer", d, false);
        ESP_LOGI(TAG, "timer %u ms", (unsigned)ms);
    }
    return e;
}

void tricks_timer_cancel(void)
{
    if (s_timer) esp_timer_stop(s_timer);
    s_timer_deadline_us = 0;
}

esp_err_t tricks_explore_start(uint32_t seconds)
{
    if (seconds < 5) seconds = 5;
    if (seconds > 120) seconds = 120;
    if (behavior_docked()) return ESP_ERR_INVALID_STATE;
    if (!foot_cliff_available())
        ESP_LOGW(TAG, "explore: no FOOT cliff — walking anyway (keep robot on a safe surface)");
    job_t j = { .kind = JOB_EXPLORE, .seconds = seconds };
    return launch(j);
}

void tricks_explore_stop(void)
{
    s_stop = true;
}

esp_err_t tricks_game_start(const char *game)
{
    if (!game || !game[0]) return ESP_ERR_INVALID_ARG;
    if (strcmp(game, "rps") && strcmp(game, "ttt") &&
        strcmp(game, "zombie") && strcmp(game, "fix_bugs"))
        return ESP_ERR_INVALID_ARG;
    job_t j = { .kind = JOB_GAME };
    strlcpy(j.game, game, sizeof(j.game));
    return launch(j);
}
