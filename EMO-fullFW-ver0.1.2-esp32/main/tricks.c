// tricks.c â€” see tricks.h.
#include "tricks.h"
#include "behavior.h"
#include "foot_cliff.h"
#include "legs.h"
#include "video.h"
#include "anim.h"
#include "microphone.h"
#include "k210_uart.h"
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

static char s_show_mode[24] = "show";

// ---- stock "shows": sequences of profile animations (body motion + face + sound) ----
// Play animation `name` (see anim.c) and wait until both its motion and its clip are over.
static void show_step(const char *name)
{
    if (s_stop) return;
    set_st(s_show_mode, name, true);
    uint32_t d = 0;
    if (anim_play(name, &d) != ESP_OK) return;
    if (d > 60000) d = 60000;
    int64_t until = esp_timer_get_time() + (int64_t)d * 1000;
    wait_ms(300);
    while (!s_stop) {
        legs_status_t m; legs_get_status(&m);
        if (!m.playing && esp_timer_get_time() >= until) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    wait_ms(150);
}

static void show_stepf(const char *fmt, int n)
{
    char b[48];
    snprintf(b, sizeof(b), fmt, n);
    show_step(b);
}

static int rnd(int lo, int hi) { return lo + (int)(esp_random() % (unsigned)(hi - lo + 1)); }

static bool arg_is(const char *arg, const char *w) { return arg && arg[0] && strstr(arg, w); }

static void show_camp(const char *arg)
{
    strlcpy(s_show_mode, "camping", sizeof(s_show_mode));
    bool all = !(arg && arg[0]);
    if (all || arg_is(arg, "tent")) show_step("Camp_tent");
    if (all || arg_is(arg, "fish")) {
        show_step("Camp_fishing_start");
        show_step("Camp_fishing_Casting");
        show_stepf("Camp_fishing_wait_loop_%d", rnd(1, 2));
        show_stepf("Camp_fishing_wait_loop_%d", rnd(1, 2));
        show_stepf("Camp_fishing_Retrieve_%d", rnd(1, 3));
        show_step("Camp_fishing_end");
    }
    if (all || arg_is(arg, "barbecue") || arg_is(arg, "bbq")) {
        show_step("Camp_barbecue_ready");
        show_step("Camp_barbecue_start");
        show_stepf("Camp_barbecue_loop%d", 1);
        show_stepf("Camp_barbecue_loop%d", 2);
        show_step("Camp_barbecue_loop_Cooked");
        show_step("Camp_barbecue_end");
    }
    if (all || arg_is(arg, "frisbee")) {
        show_step("Camp_frisbee_throw");
        show_step("Camp_frisbee_run");
        show_step("Camp_frisbee_catch");
    }
    if (all || arg_is(arg, "fire")) {
        show_step("campfire_start");
        show_step("campfire_loop1");
        show_step("campfire_end");
    }
}

static void show_fireworks(void)
{
    strlcpy(s_show_mode, "fireworks", sizeof(s_show_mode));
    int o[3] = { 1, 2, 3 };                                  // all three bursts, random order
    for (int i = 2; i > 0; i--) { int k = rnd(0, i), t = o[i]; o[i] = o[k]; o[k] = t; }
    for (int i = 0; i < 3 && !s_stop; i++) show_stepf("game_firework_%d", o[i]);
    show_step("game_firework_end");
}

static void show_spray(void)
{
    strlcpy(s_show_mode, "spray", sizeof(s_show_mode));
    show_step("Spray_paint_start");
    show_step("Spray_paint_stay_1");
    for (int round = 0; round < 3 && !s_stop; round++) {
        const char *side = (esp_random() & 1) ? "left" : "right";
        char n[48];
        snprintf(n, sizeof(n), "Spray_paint_stay_%s_2", side); show_step(n);
        bool hit = (esp_random() % 100) < 60;
        snprintf(n, sizeof(n), hit ? "Spray_paint_hit_%s" : "Spray_paint_miss_%s", side); show_step(n);
        if (round == 2 || !hit) {
            if (hit) snprintf(n, sizeof(n), "Spray_paint_end_%s%d", side, rnd(1, 3));
            else     snprintf(n, sizeof(n), "Spray_paint_end_miss%d", rnd(1, 3));
            show_step(n);
            return;
        }
    }
}

static void show_dice(void)
{
    static const char *w[] = { "one", "two", "three", "four", "five", "six" };
    strlcpy(s_show_mode, "dice", sizeof(s_show_mode));
    char n[32];
    snprintf(n, sizeof(n), "dice_%s_%d", w[rnd(0, 5)], rnd(1, 2));
    show_step(n);
}

static void show_fight(const char *arg)
{
    static const char *f[] = { "Fight_Boomerang_miss", "Fight_Radish_knife", "Fight_Slingshot_hit",
                               "Fight_Slingshot_miss", "Fight_Water_Bubble_gun" };
    strlcpy(s_show_mode, "fight", sizeof(s_show_mode));
    if (arg_is(arg, "boom")) show_step(f[0]);
    else if (arg_is(arg, "radish") || arg_is(arg, "knife")) show_step(f[1]);
    else if (arg_is(arg, "sling")) show_step(f[rnd(2, 3)]);
    else if (arg_is(arg, "bubble") || arg_is(arg, "water")) show_step(f[4]);
    else show_step(f[rnd(0, 4)]);
}

static void show_fit(void)
{
    strlcpy(s_show_mode, "exercise", sizeof(s_show_mode));
    show_step("Fit_start");
    show_step("Fit_321");
    static const struct { const char *fmt; int n; } blocks[] = {
        { "Fit_Warmup_loop%d", 4 }, { "Fit_Aerobic_loop%d", 4 }, { "Fit_Core_loop%d", 6 },
        { "Fit_Power_loop%d", 6 }, { "Fit_Relax_loop%d", 5 },
    };
    for (size_t b = 0; b < sizeof(blocks) / sizeof(blocks[0]) && !s_stop; b++) {
        for (int r = 0; r < 3 && !s_stop; r++) show_stepf(blocks[b].fmt, rnd(1, blocks[b].n));
        if (b + 1 < sizeof(blocks) / sizeof(blocks[0])) show_step((b & 1) ? "Fit_Rest" : "Fit_Next");
    }
    show_step("Fit_Good");
    show_step("Fit_end");
}

static void show_drive(const char *arg)
{
    strlcpy(s_show_mode, "drive", sizeof(s_show_mode));
    if (arg_is(arg, "bike") || arg_is(arg, "bicycle")) show_step("Drive_bicycle");
    else if (arg_is(arg, "car")) show_step("Drive_car");
    else if (arg_is(arg, "ship") || arg_is(arg, "boat")) show_step("Drive_ship");
    else { static const char *d[] = { "Drive_bicycle", "Drive_car", "Drive_ship" }; show_step(d[rnd(0, 2)]); }
}

static void show_band(const char *arg)
{
    static const char *inst[] = { "drum", "maraca", "piano", "tambourine", "trumpet" };
    strlcpy(s_show_mode, "band", sizeof(s_show_mode));
    int pick = -1;
    for (int i = 0; i < 5; i++) if (arg_is(arg, inst[i])) pick = i;
    char n[40];
    if (pick < 0) {                                        // play together: ensemble part of one instrument
        static const char *e[] = { "emo_ensemble_drum_1", "emo_ensemble_maraca_1", "emo_ensemble_piano_1" };
        show_step(e[rnd(0, 2)]);
        return;
    }
    snprintf(n, sizeof(n), pick == 4 ? "emo_%s_reday1" : "emo_%s_ready1", inst[pick]); show_step(n);
    if (pick == 2) snprintf(n, sizeof(n), "emo_piano_%d", rnd(1, 2));
    else if (pick <= 1) snprintf(n, sizeof(n), "emo_ensemble_%s_1", inst[pick]);   // no solo clip in the profile
    else snprintf(n, sizeof(n), "emo_%s_1", inst[pick]);
    show_step(n);
    snprintf(n, sizeof(n), "emo_%s_eixt1", inst[pick]); show_step(n);
}

static void camping_body(void)
{
    set_st("camping", "running", true);
    show_camp(NULL);
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

// Stock "hands up" game (gesture_gun_*). The robot raises its hands and waits for
// a loud sound (clap / "bang!") from the player, measured on the K210 mic; then it
// either dodges (~71 %) or gets to shoot back (~29 %, stock odds). With no sound
// for GUN_WAIT_MS it just lowers its hands.
#define GUN_WAIT_MS   20000
__attribute__((unused)) static bool gun_wait_for_bang(uint32_t y0)
{
    // The hands-up face plays its own sound, which the mic hears: do not arm until
    // the room has been quiet for 1.5 s in a row (max 12 s), else we trigger on ourselves.
    wait_ms(1500);
    if (s_stop) return false;
    microphone_open();
    uint16_t base = 0;
    int quiet = 0;
    int64_t lim = esp_timer_get_time() + 12000000;
    while (!s_stop && quiet < 15 && esp_timer_get_time() < lim) {
        microphone_stats_t st; microphone_get_stats(&st);
        if (!st.open) microphone_open();
        if (behavior_yolo_count() != y0) { ESP_LOGI("tricks", "gun: K210 YOLO event"); microphone_mark_closed(); k210_uart_send_mode(K210_MODE_IDLE); k210_link_set_state(LINK_IDLE); return true; }
        if (st.peak < 3500) { quiet++; if (st.peak > base) base = st.peak; }
        else { quiet = 0; base = 0; }
        wait_ms(100);
    }
    if (s_stop) return false;
    uint32_t thr = (uint32_t)base * 3 / 2 + 3000;
    if (thr < 7000) thr = 7000;
    if (thr > 24000) thr = 24000;
    ESP_LOGI("tricks", "gun: waiting for a bang (room peak %u, trigger > %u)", (unsigned)base, (unsigned)thr);
    int64_t until = esp_timer_get_time() + (int64_t)GUN_WAIT_MS * 1000;
    bool heard = false;
    while (!s_stop && esp_timer_get_time() < until) {
        microphone_stats_t st; microphone_get_stats(&st);
        if (!st.open) microphone_open();                   // K210 VAD closed the window
        if (behavior_yolo_count() != y0) { heard = true; ESP_LOGI("tricks", "gun: K210 YOLO event"); break; }
        if (st.peak > thr) { heard = true; ESP_LOGI("tricks", "gun: bang! peak=%u", (unsigned)st.peak); break; }
        wait_ms(30);
    }
    microphone_mark_closed();
    k210_uart_send_mode(K210_MODE_IDLE);                   // wake word armed again
    k210_link_set_state(LINK_IDLE);
    return heard;
}

static void game_gun(void)
{
    set_st("game", "gun", true);
    bool ack = false;
    // Raise-hands clip (with sound), then the shaking-hands loop (with sound), then listen for a bang.
    video_play_face("gesture_gun_don't_move_start", 0, true, &ack);   // hands up from below
    ESP_LOGI("tricks", "gun: hands-up face ack=%d", (int)ack);
    bool bang = false;
    wait_ms(3000);                                         // let the 'start' clip finish first
    video_play_face("gesture_gun_don't_move_loop", 1, true, &ack);
    ESP_LOGI("tricks", "gun: loop face ack=%d, listening for a bang for 40 s", (int)ack);
    // Sound trigger (no camera): learn how loud the loop itself is for 2 s,
    // then fire on anything clearly louder (clap / "bang").
    microphone_open();
    uint16_t base = 0;
    for (int i = 0; i < 20 && !s_stop; i++) {
        microphone_stats_t st; microphone_get_stats(&st);
        if (!st.open) microphone_open();
        if (st.peak > base) base = st.peak;
        wait_ms(100);
    }
    uint32_t thr = (uint32_t)base * 3 / 2 + 4000;
    if (thr < 7000) thr = 7000;
    if (thr > 28000) thr = 28000;
    ESP_LOGI("tricks", "gun: loop sound peak %u, trigger > %u", (unsigned)base, (unsigned)thr);
    int64_t until = esp_timer_get_time() + 40000000LL;
    while (!s_stop && esp_timer_get_time() < until) {
        microphone_stats_t st; microphone_get_stats(&st);
        if (!st.open) microphone_open();
        if (st.peak > thr) { bang = true; ESP_LOGI("tricks", "gun: bang! peak=%u", (unsigned)st.peak); break; }
        wait_ms(30);
    }
    microphone_mark_closed();
    k210_uart_send_mode(K210_MODE_IDLE);
    k210_link_set_state(LINK_IDLE);
    if (s_stop) return;
    if (!bang) {                                          // nobody shot: lower the hands
        play_mot("gesture_gun_end"); wait_motion_or_stop();
        return;
    }
    if (esp_random() % 100 < 29) {
        play_mot("gesture_gun");  wait_motion_or_stop();
        if (s_stop) return;
        play_mot("gesture_gun_stay"); wait_motion_or_stop();
        if (s_stop) return;
        play_mot("gesture_gun_end");  wait_motion_or_stop();
    } else {
        play_mot((esp_random() & 1) ? "gesture_gun_dodge1" : "gesture_gun_dodge2");
        wait_motion_or_stop();
    }
}
typedef struct {
    enum { JOB_CAMP, JOB_EXPLORE, JOB_GAME } kind;
    uint32_t seconds;
    char game[32];
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
        else if (!strcmp(job.game, "gun")) game_gun();
        else {
            char g[32]; strlcpy(g, job.game, sizeof(g));
            char *arg = strchr(g, ':'); if (arg) *arg++ = 0;
            if (!strcmp(g, "fireworks")) show_fireworks();
            else if (!strcmp(g, "spray")) show_spray();
            else if (!strcmp(g, "dice")) show_dice();
            else if (!strcmp(g, "fight")) show_fight(arg);
            else if (!strcmp(g, "fit")) show_fit();
            else if (!strcmp(g, "drive")) show_drive(arg);
            else if (!strcmp(g, "camp")) show_camp(arg);
            else if (!strcmp(g, "band")) show_band(arg);
        }
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
    ESP_LOGI(TAG, "timer fired â†’ alarm");
    set_st("timer", "alarm", true);
    play_mot("alarm_shock_start_1");
    // short ring; end clip after a beat via another one-shot-ish delay in task would be nicer,
    // but keep ISR light â€” schedule end on a tiny task.
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
        ESP_LOGW(TAG, "explore: no FOOT cliff â€” walking anyway (keep robot on a safe surface)");
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
        strcmp(game, "zombie") && strcmp(game, "fix_bugs") && strcmp(game, "gun")) {
        static const char *k_shows[] = { "fireworks", "spray", "dice", "fight", "fit", "drive", "camp", "band" };
        bool okk = false;
        for (size_t i = 0; i < sizeof(k_shows) / sizeof(k_shows[0]); i++) {
            size_t n = strlen(k_shows[i]);
            if (!strncmp(game, k_shows[i], n) && (game[n] == 0 || game[n] == ':')) okk = true;
        }
        if (!okk || strlen(game) >= sizeof(((job_t *)0)->game)) return ESP_ERR_INVALID_ARG;
    }
    job_t j = { .kind = JOB_GAME };
    strlcpy(j.game, game, sizeof(j.game));
    return launch(j);
}
