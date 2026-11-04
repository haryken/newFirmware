// behavior.c — see behavior.h.
#include "behavior.h"
#include "foot_cliff.h"
#include "touch_pet.h"
#include "legs.h"
#include "video.h"
#include "k210_boot.h"
#include "k210_link.h"
#include "settings.h"
#include "battery.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "behavior";

static behavior_status_t s_st = {
    .idle_faces = true, .cliff_enable = true, .petting_enable = true, .yolo_react = true,
    .explore_enable = true,
};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_yolo_cool_us;
static int64_t s_idle_next_us;
static int64_t s_cliff_cool_us;
static int64_t s_explore_next_us;
static bool s_petting;
static int64_t s_pet_end_us;
static volatile bool s_voice_busy;
static volatile bool s_touch_wake;   // touch while docked: wake from sleep face

static const char *IDLE_FACES[] = {
    "stay_wink_1", "stay_a_b", "stay_c_d", "look_around_1", "look_around_2",
};
#define N_IDLE (sizeof(IDLE_FACES) / sizeof(IDLE_FACES[0]))

static void set_mode(const char *m)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_st.mode, m, sizeof(s_st.mode));
    portEXIT_CRITICAL(&s_mux);
}

static void play_face(const char *name, bool mp3)
{
    if (!k210_boot_done() || k210_link_booting()) return;
    if (k210_link_state() == LINK_SPEAK) return;
    bool ack = false;
    video_play_face(name, 0, mp3, &ack);
}

static void play_mot(const char *name)
{
    legs_opts_t o = LEGS_OPTS_DEFAULT;   // media=true by default
    if (legs_play(name, o) != ESP_OK)
        play_face(name, true);           // face/mp3 only if .mot missing
}

// On the charger the legs go limp (no PWM) so the robot can't push itself off.
static void go_limp(void)
{
    legs_stop();
    legs_release();
}

static void cliff_react(uint8_t mask)
{
    int64_t now = esp_timer_get_time();
    if (now < s_cliff_cool_us) return;
    s_cliff_cool_us = now + 2500000;
    set_mode("cliff");
    legs_stop();
    // Stock cliff_edge_react: mask 3 -> cliff_L, 0xC -> cliff_R, every other mask
    // (single sensor, diagonal, front/rear pair) -> back away + a face with sound.
    if (mask == 0x3) play_mot("cliff_L");
    else if (mask == 0xC) play_mot("cliff_R");
    else {
        char nm[24];
        snprintf(nm, sizeof(nm), "explore_back_away%u", (unsigned)(1 + esp_random() % 3));
        legs_opts_t o = LEGS_OPTS_DEFAULT;
        o.media = false;
        if (legs_play(nm, o) != ESP_OK) play_mot("cliff_B");
        else play_face("mood_wronged", true);
    }
    ESP_LOGW(TAG, "cliff mask=0x%02x", mask);
}

// Cliff / lift handling (thresholds from stock foot_cliff_calibrate: 30 % of floor reading).
//  - 3 or 4 feet off the ground  -> picked up: kick legs (react_pick_up*), then put_down.
//  - 1 or 2 feet over nothing    -> edge: cliff_L / cliff_R / cliff_B motion.
static bool s_lifted, s_lift_limp, s_need_cal = true;
static int64_t s_cal_at_us, s_lift_react_us;
static uint8_t s_last_mask;

static void cliff_tick(void)
{
    foot_cliff_t f;
    if (foot_cliff_poll(&f) != ESP_OK) return;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    s_st.cliff_mask = f.mask;
    memcpy(s_st.cliff_ch, f.ch, sizeof(s_st.cliff_ch));
    portEXIT_CRITICAL(&s_mux);

    if (f.mask != s_last_mask) {
        ESP_LOGW(TAG, "cliff mask 0x%x -> 0x%x ch=%u,%u,%u,%u thr=%u,%u,%u,%u", s_last_mask, f.mask,
                 f.ch[0], f.ch[1], f.ch[2], f.ch[3], f.thr_ch[0], f.thr_ch[1], f.thr_ch[2], f.thr_ch[3]);
        s_last_mask = f.mask;
    }

    // (Re)calibrate on the ground: first boot, after undock, after a reaction.
    if (s_need_cal && f.mask == 0 && now >= s_cal_at_us) {
        if (foot_cliff_calibrate() == ESP_OK) s_need_cal = false;
        else s_cal_at_us = now + 2000000;
    }
    if (!f.calibrated || s_st.docked || s_petting) return;

    int bits = __builtin_popcount(f.mask);
    if (bits >= 3) {
        if (!s_lifted) { s_lifted = true; s_lift_react_us = 0; set_mode("lifted"); }
        if (s_voice_busy) {            // talking to Xiaozhi: stay still, motors off; kick only after the chat is over
            if (!s_lift_limp) { go_limp(); s_lift_limp = true; ESP_LOGI(TAG, "lifted during chat: legs limp"); }
            s_lift_react_us = 0;
            return;
        }
        s_lift_limp = false;
        if (now >= s_lift_react_us) {
            s_lift_react_us = now + 3500000;
            legs_stop();
            char nm[24];
            snprintf(nm, sizeof(nm), "react_pick_up%u", (unsigned)(1 + esp_random() % 5));
            play_mot(nm);
            ESP_LOGW(TAG, "lifted -> %s", nm);
        }
        return;
    }
    if (s_lifted && f.mask == 0) {
        s_lifted = false;
        legs_stop();
        if (s_voice_busy) {            // put down while chatting: just stand, no reaction
            static const uint8_t home[4] = {90, 90, 90, 90};
            s_lift_limp = false;
            legs_pose(home, 400);
            set_mode("idle");
            s_need_cal = true; s_cal_at_us = now + 4000000;
            return;
        }
        play_mot((esp_random() & 1) ? "react_put_down_1" : "react_put_down_2");
        set_mode("idle");
        s_need_cal = true; s_cal_at_us = now + 4000000;
        s_explore_next_us = now + 4000000;
        return;
    }
    if (f.mask) {
        cliff_react(f.mask);
        s_need_cal = true; s_cal_at_us = now + 6000000;
    }
}
static void pet_tick(bool touched)
{
    int64_t now = esp_timer_get_time();
    if (s_st.docked) { if (touched) s_touch_wake = true; return; }
    if (!s_st.petting_enable) return;
    if (touched) {
        if (!s_petting) {
            s_petting = true;
            set_mode("petting");
            legs_stop();
            play_mot("touch_get_in");
            ESP_LOGI(TAG, "petting start");
        }
        s_pet_end_us = now + 800000;   // release debounce
        // occasional loop face while held
        static int64_t next_loop;
        if (now > next_loop) {
            next_loop = now + 3500000;
            play_face((esp_random() & 1) ? "touch_loop_enjoy" : "touch_loop", true);
        }
    } else if (s_petting && now > s_pet_end_us) {
        s_petting = false;
        play_mot("touch_get_out");
        set_mode("idle");
        ESP_LOGI(TAG, "petting end");
    }
    portENTER_CRITICAL(&s_mux);
    s_st.petting = s_petting;
    portEXIT_CRITICAL(&s_mux);
}

static void idle_tick(void)
{
    if (!s_st.idle_faces || s_st.docked || s_petting) return;
    if (s_st.explore_enable) return;   // explore owns "alive" when enabled
    if (!k210_boot_done()) return;
    legs_status_t m; legs_get_status(&m);
    if (m.playing) return;
    if (k210_link_state() != LINK_IDLE) return;
    int64_t now = esp_timer_get_time();
    if (now < s_idle_next_us) return;
    s_idle_next_us = now + (int64_t)(12000000 + (esp_random() % 10000000));
    const char *f = IDLE_FACES[esp_random() % N_IDLE];
    play_face(f, false);
    set_mode("idle");
}

// Desk wander: procedural gait with boosted amplitude (stock .mot clips are tiny
// / easy to miss, especially while the feet are still on the charger contacts).
static void explore_tick(void)
{
    if (!s_st.explore_enable || s_st.docked || s_petting || s_lifted) return;
    if (!k210_boot_done()) return;
    if (k210_link_state() == LINK_SPEAK) return;
    legs_status_t m; legs_get_status(&m);
    if (m.playing) return;
    int64_t now = esp_timer_get_time();
    if (now < s_explore_next_us) return;

    uint32_t r = esp_random();
    bool turn = (r % 3) == 0;
    bool reverse = (r & 1) != 0;
    legs_gait_t g;
    legs_gait_preset(turn, reverse, &g);
    g.cycles = 3.0f + (float)(r % 3);   // 3..5 periods — long enough to see
    g.neutral_after = true;
    // Slower than the stock preset (stock T = 400..600 ms looked too fast on
    // the desk): stretch the period, and boost the amplitude a bit less.
    g.period_ms = (uint32_t)((uint64_t)g.period_ms * CONFIG_EMO_EXPLORE_PERIOD_PCT / 100);
    if (g.period_ms > 5000) g.period_ms = 5000;
    for (int i = 0; i < 4; i++) {
        int a = (int)g.A[i] * CONFIG_EMO_EXPLORE_AMP_PCT / 100;
        if (a > 24) a = 24;
        if (a < -24) a = -24;
        if (a == 0) a = turn ? 12 : 14;
        g.A[i] = (int8_t)a;
    }

    set_mode("explore");
    esp_err_t e = legs_gait(&g);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "explore gait fail %s — fallback mot", esp_err_to_name(e));
        play_mot(turn ? (reverse ? "explore_turn_right1" : "explore_turn_left1")
                      : (reverse ? "explore_back_away1" : "explore_foward1"));
    } else {
        play_face(turn ? "look_around_1" : "look_around_2", false);
    }
    s_explore_next_us = now + (int64_t)(600000 + (esp_random() % 900000));
    ESP_LOGI(TAG, "explore gait turn=%d rev=%d cycles=%.0f A=%d/%d/%d/%d",
             turn, reverse, g.cycles, g.A[0], g.A[1], g.A[2], g.A[3]);
}

// Charger detect = GPIO36 "sense" (stock power_object input). Measured over
// Wi-Fi with the USB cable unplugged: high while a charger is attached, low on
// battery (battery reading also drops ~200 mV). Debounced.
static bool s_charger, s_chg_raw, s_chg_init;
static int64_t s_chg_change_us;
static int64_t s_dock_since_us;
static bool s_limp_done;
static bool s_sleeping;
static int64_t s_sleep_next_us;
static unsigned s_sleep_clip;

static bool charger_present(void)
{
#if CONFIG_EMO_AUTO_DOCK
    battery_t b; battery_get(&b);
    if (!b.valid) return s_charger;
    int64_t now = esp_timer_get_time();
    if (!s_chg_init) {                       // first reading: no debounce
        s_chg_init = true;
        s_chg_raw = s_charger = b.sense;
        s_chg_change_us = now;
    }
    if (b.sense != s_chg_raw) { s_chg_raw = b.sense; s_chg_change_us = now; }
    if (s_chg_raw != s_charger && now - s_chg_change_us >= 2500000) {
        s_charger = s_chg_raw;
        ESP_LOGI(TAG, "charger %s", s_charger ? "connected" : "removed");
    }
#endif
    return s_charger;
}

static void dock_tick(void)
{
    // docked = web/NVS checkbox OR charger attached.
    emo_settings_t st; settings_get_copy(&st);
    bool dock = st.docked || charger_present();
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    bool changed = s_st.docked != dock;
    s_st.docked = dock;
    portEXIT_CRITICAL(&s_mux);
    if (changed) {
        if (dock) {
            // Stand still in the waiting pose, go limp after DOCK_LIMP_MS.
            ESP_LOGI(TAG, "docked: waiting pose, limp in %d s", CONFIG_EMO_DOCK_LIMP_S);
            legs_stop();
            const uint8_t p[4] = { 90, 90, 90, 90 };
            legs_pose(p, 500);
            s_dock_since_us = now;
            s_limp_done = false;
            s_sleeping = false;
            play_face("charge_get_in_new", true);
            set_mode("docked");
        } else {
            ESP_LOGI(TAG, "undocked: explore soon");
            if (s_sleeping) { s_sleeping = false; play_face("sleep_wake_up_fast", false); }
            s_explore_next_us = now + 1500000;
            s_need_cal = true; s_cal_at_us = now + 4000000;
            s_limp_done = false;
            set_mode("idle");
        }
    }
    if (!dock) return;
    if (!s_limp_done && now - s_dock_since_us >= (int64_t)CONFIG_EMO_DOCK_LIMP_S * 1000000) {
        ESP_LOGI(TAG, "docked %d s: legs limp", CONFIG_EMO_DOCK_LIMP_S);
        go_limp();
        s_limp_done = true;
    }
    // Sleep face after a while on the charger (stock: charge_get_in_new, then the sleep_* clips).
#if CONFIG_EMO_DOCK_SLEEP_S > 0
    {
        static const char *const LOOPS[] = { "sleep_loop_1", "sleep_breath_1", "sleep_Fireflies_1", "sleep_loop_2", "sleep_breath_2", "sleep_Fireflies_2" };
        bool busy = s_voice_busy || k210_link_state() == LINK_SPEAK;
        if (busy) {
            // a conversation owns the face: restart the idle timer, sleep again later
            s_sleeping = false;
            s_dock_since_us = now;
            s_touch_wake = false;
        } else if (s_touch_wake) {
            s_touch_wake = false;
            if (s_sleeping) {
                ESP_LOGI(TAG, "touch: wake from sleep");
                s_sleeping = false;
                play_face("sleep_wake_up_fast", false);
                set_mode("docked");
            }
            s_dock_since_us = now;   // sleep again after another idle period
        } else if (!s_sleeping && now - s_dock_since_us >= (int64_t)CONFIG_EMO_DOCK_SLEEP_S * 1000000) {
            ESP_LOGI(TAG, "docked %d s: sleep face", CONFIG_EMO_DOCK_SLEEP_S);
            s_sleeping = true;
            s_sleep_clip = 0;
            play_face("sleep_get_in_slow", false);
            s_sleep_next_us = now + 9000000;   // let the get-in clip finish, then loop
            set_mode("sleeping");
        } else if (s_sleeping && now >= s_sleep_next_us) {
            const char *c = LOOPS[s_sleep_clip++ % (sizeof(LOOPS) / sizeof(LOOPS[0]))];
            if (s_sleep_clip == 1) c = LOOPS[0];
            video_play_face(c, 1, false, NULL);   // loop until replaced
            s_sleep_next_us = now + 60000000;     // vary the clip every minute
        }
    }
#endif
    legs_status_t m; legs_get_status(&m);
    if (m.playing) {
        if (strstr(m.name, "explore") || strstr(m.name, "gait") || strstr(m.name, "walk") ||
            strstr(m.name, "foward") || strstr(m.name, "turn")) {
            go_limp();
            s_limp_done = true;
        }
    }
}

static void task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (!k210_boot_done()) vTaskDelay(pdMS_TO_TICKS(200));

    s_st.foot_ok = foot_cliff_available();
    s_st.touch_ok = touch_pet_available();
    emo_settings_t st; settings_get_copy(&st);
    s_st.docked = st.docked;
    s_st.idle_faces = st.idle_faces;
    s_st.cliff_enable = st.cliff_enable;
    s_st.petting_enable = st.petting_enable;
    s_st.yolo_react = st.yolo_react;
    s_st.explore_enable = st.explore_enable;
    strlcpy(s_st.mode, s_st.docked ? "docked" : "idle", sizeof(s_st.mode));
    s_idle_next_us = esp_timer_get_time() + 8000000;
    s_explore_next_us = esp_timer_get_time() + 2500000;
    ESP_LOGI(TAG, "running (foot=%d touch=%d docked=%d explore=%d)",
             s_st.foot_ok, s_st.touch_ok, s_st.docked, s_st.explore_enable);

    for (;;) {
        settings_get_copy(&st);
        s_st.idle_faces = st.idle_faces;
        s_st.cliff_enable = st.cliff_enable;
        s_st.petting_enable = st.petting_enable;
        s_st.yolo_react = st.yolo_react;
        s_st.explore_enable = st.explore_enable;

        dock_tick();

        if (s_st.cliff_enable && s_st.foot_ok) cliff_tick();
        if (s_st.touch_ok) {
            touch_pet_t t;
            if (touch_pet_poll(&t) == ESP_OK) pet_tick(t.touched);
        }

        if (s_voice_busy) {
            // Xiaozhi session: freeze wander so legs/faces don't fight mic/WS.
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        explore_tick();
        idle_tick();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t behavior_start(void)
{
    foot_cliff_start();   // may fail → cliff disabled
    touch_pet_start();    // may fail → petting disabled
    if (xTaskCreate(task, "behavior", 6144, NULL, 4, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void behavior_get_status(behavior_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

void behavior_on_yolo(uint8_t cls, uint8_t x, uint8_t y)
{
    (void)x; (void)y;
    if (!s_st.yolo_react || s_st.docked || s_petting) return;
    int64_t now = esp_timer_get_time();
    if (now < s_yolo_cool_us) return;
    s_yolo_cool_us = now + 8000000;
    ESP_LOGI(TAG, "yolo class=%u → see_EMO", cls);
    play_face("see_EMO", true);
}

void behavior_set_docked(bool on)
{
    emo_settings_t st; settings_get_copy(&st);
    st.docked = on;
    settings_save(&st);
}

bool behavior_docked(void)
{
    emo_settings_t st; settings_get_copy(&st);
    portENTER_CRITICAL(&s_mux);
    bool eff = s_st.docked;              // checkbox OR charger attached
    portEXIT_CRITICAL(&s_mux);
    return st.docked || eff;
}

void behavior_set_voice_busy(bool busy)
{
    s_voice_busy = busy;
    if (busy) {
        legs_stop();
        set_mode("voice");
    }
}
