// behavior.c — see behavior.h.
#include "behavior.h"
#include "foot_cliff.h"
#include "touch_pet.h"
#include "legs.h"
#include "video.h"
#include "k210_boot.h"
#include "k210_link.h"
#include "settings.h"
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

static void hold_neutral(void)
{
    legs_stop();
    const uint8_t p[4] = { 90, 90, 90, 90 };
    legs_pose(p, 400);
}

static void cliff_react(uint8_t mask)
{
    int64_t now = esp_timer_get_time();
    if (now < s_cliff_cool_us) return;
    s_cliff_cool_us = now + 2500000;
    set_mode("cliff");
    legs_stop();
    if (mask == 0x3) play_mot("cliff_L");
    else if (mask == 0xC) play_mot("cliff_R");
    else if (mask & 0x3) play_mot("cliff_L");
    else if (mask & 0xC) play_mot("cliff_R");
    else play_mot("cliff_B");
    ESP_LOGW(TAG, "cliff mask=0x%02x", mask);
}

static void pet_tick(bool touched)
{
    int64_t now = esp_timer_get_time();
    if (!s_st.petting_enable || s_st.docked) return;
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
    if (!s_st.explore_enable || s_st.docked || s_petting) return;
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
    for (int i = 0; i < 4; i++) {
        int a = (int)g.A[i] * 2;         // ~2x stock amp so desk walk is obvious
        if (a > 28) a = 28;
        if (a < -28) a = -28;
        if (a == 0) a = turn ? 14 : 18;
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

static void dock_tick(void)
{
    // GPIO36 "bat_sense" is a divider select on this board (often stuck high) —
    // NOT a reliable charger detect. Dock is web/NVS checkbox only.
    emo_settings_t st; settings_get_copy(&st);
    bool dock = st.docked;
    portENTER_CRITICAL(&s_mux);
    bool changed = s_st.docked != dock;
    s_st.docked = dock;
    portEXIT_CRITICAL(&s_mux);
    if (changed) {
        if (dock) {
            ESP_LOGI(TAG, "docked: hold pose (stop explore)");
            hold_neutral();
            play_face("charge_get_in_new", true);
            set_mode("docked");
        } else {
            ESP_LOGI(TAG, "undocked: big walk soon");
            s_explore_next_us = esp_timer_get_time() + 400000;
            set_mode("idle");
        }
    }
    if (!dock) return;
    legs_status_t m; legs_get_status(&m);
    if (m.playing) {
        if (strstr(m.name, "explore") || strstr(m.name, "gait") || strstr(m.name, "walk") ||
            strstr(m.name, "foward") || strstr(m.name, "turn")) {
            hold_neutral();
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

        if (s_st.cliff_enable && s_st.foot_ok && !s_st.docked && !s_petting) {
            foot_cliff_t f;
            if (foot_cliff_poll(&f) == ESP_OK) {
                portENTER_CRITICAL(&s_mux);
                s_st.cliff_mask = f.mask;
                memcpy(s_st.cliff_ch, f.ch, sizeof(s_st.cliff_ch));
                portEXIT_CRITICAL(&s_mux);
                if (f.mask) {
                    legs_status_t m; legs_get_status(&m);
                    if (m.playing) cliff_react(f.mask);
                }
            }
        }

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
    return st.docked;
}

void behavior_set_voice_busy(bool busy)
{
    s_voice_busy = busy;
    if (busy) {
        legs_stop();
        set_mode("voice");
    }
}
