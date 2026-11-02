// legs.h — leg motion: .mot playback and the procedural gait, as the stock
// ESP32 firmware does it.
//
// Sources: the stock firmware decompile (emo-new-firmware/ghidraExtracted-
// elfPartition1.c: motion_parse_file FUN_40143b78, animation_player
// FUN_40143250, servo move FUN_4025b134 / FUN_4025a504, gait generators
// FUN_4025c6b8 / FUN_4025d338 / FUN_4025a93c) and the 582 .mot files
// themselves (all frames validated). Servo output: servo.h.
//
// .mot file = N x 20-byte frames:
//   [0..3] 55 AA 55 AA  [4] type 02 (0E = "eyes" variant)  [5] mode
//   [6..9] gait params   [10] gait param   [11..14] servo degrees  [15] 0
//   [16..17] duration ms LE   [18] sum(bytes 4..17) & 0xFF   [19] ED
//   mode FE = move all four servos to [11..14], linearly over duration (20 ms ticks)
//   mode FD = skip
//   mode 01 / 02 = stock walk / turn oscillator (reproduced, see legs_gait_t)
//   other   = other stock generators (dances): pose held for the duration
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     fs_ok;          // /spiffs mounted
    bool     playing;
    bool     loop;
    char     name[40];       // current / last motion
    char     media[40];      // face/sound asset started with it ("" = none)
    uint16_t frame, frames;  // progress of the current motion
    uint32_t unsupported;    // held frames (unknown stock generator) in this motion
    uint8_t  pos[4];         // commanded angles, degrees
    bool     powered;        // PWM running (false = servos limp)
    uint32_t plays;          // motions started since boot
    uint16_t files;          // .mot files found
    uint32_t fs_total, fs_used;
    float    speed;          // playback speed factor of the current motion (1 = file timing)
    float    speed_default;  // factor used when a request gives none (settings: motion_speed %)
    uint32_t expected_ms;    // file time at that speed (gait frames = cycles x period)
    uint32_t elapsed_ms;     // wall time of the current/last playback
    uint32_t music_ms;       // length of the paired song (0 = none), from mot_media.txt
} legs_status_t;

// Mount SPIFFS ("storage" partition at /spiffs) and index /spiffs/mot. servo_init() first.
esp_err_t legs_init(void);

// Called when a motion starts with media: play face asset `name` (+ its mp3) on
// the K210, like the stock behaviour engine (app_main wires this to video_play_face).
typedef void (*legs_media_cb_t)(const char *name, bool with_mp3);
void legs_set_media_cb(legs_media_cb_t cb);

typedef struct {
    bool  loop;       // repeat until stopped
    bool  media;      // also start the paired face animation / music (mot_media.txt, else same name)
    float speed;      // 1.0 = frame timing from the file; 2.0 = twice as fast (0.25..4); 0 = default
    bool  fit_music;  // speed = file time / song length, so the legs end with the music
} legs_opts_t;
#define LEGS_OPTS_DEFAULT ((legs_opts_t){ .loop = false, .media = true, .speed = 0.0f, .fit_music = false })

// Speed used when a request gives none (settings: motion_speed, percent). Measured
// against the stock dances, the file timing already matches the songs (d1_EmoDance
// 40.0 s of frames vs a 40.1 s song; median over 13 dances 0.97), so 1.0 = authored
// timing. Clamped to 0.25..4.
void  legs_set_default_speed(float speed);
float legs_default_speed(void);

// Play /spiffs/mot/<name>.mot in a background task (stops a running motion first).
// ESP_ERR_NOT_FOUND: no such file. ESP_ERR_INVALID_ARG: bad name.
// ESP_ERR_INVALID_SIZE: file has no valid frames. ESP_ERR_INVALID_STATE: no SPIFFS.
esp_err_t legs_play(const char *name, legs_opts_t opts);

// Procedural gait (the stock firmware's Otto-style oscillator, used by the .mot
// gait frames and by walk/turn requests): every 20 ms each servo goes to
// 90 + A[i]*sin(phase + phase0[i]) + O[i], phase advancing 2*pi per period.
typedef struct {
    float    cycles;       // periods to run (fractional allowed)
    uint32_t period_ms;    // one period (T)
    int8_t   A[4];         // amplitude per servo: left legs, right legs, left foot, right foot
    int8_t   O[4];         // offset per servo
    float    phase0[4];    // phase offset per servo, degrees (feet usually -90 or +90)
    float    dir_deg;      // initial phase of all servos, 0 or 180 (mirrors the gait)
    bool     neutral_after;// return to 90/90/90/90 over period/2 when done
} legs_gait_t;

// Stock parameters for walking and turning (from the explore_* motion files);
// cycles is filled by the caller. reverse: backward / right.
void legs_gait_preset(bool turn, bool reverse, legs_gait_t *out);

// Run a gait in the background (stops a running motion first).
esp_err_t legs_gait(const legs_gait_t *g);

// Stop the current motion (returns once the task has ended). Servos keep holding.
void legs_stop(void);

// Move the four servos to deg[4] over ms milliseconds (blocking, max 10 s).
// hold=true: keep PWM after the move (no idle release) — for desk twitch tests.
esp_err_t legs_pose(const uint8_t deg[4], uint32_t ms);
esp_err_t legs_pose_hold(const uint8_t deg[4], uint32_t ms, bool hold);

// Stop the PWM: servos go limp (stock "stop_all"). Any move powers them again.
void legs_release(void);

void legs_get_status(legs_status_t *out);

// JSON array of the motion names (without ".mot"), sorted. Owned by the module.
const char *legs_list_json(void);

#ifdef __cplusplus
}
#endif
