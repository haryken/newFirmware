// behavior.h — safe stock-like behaviours on top of legs/video (no K210 flash).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     docked;            // hold pose, no walk react explore
    bool     idle_faces;        // periodic stay_* / wink
    bool     cliff_enable;      // interrupt motion on FOOT cliff
    bool     petting_enable;
    bool     yolo_react;        // play see_EMO on YOLO (cooldown)
    bool     explore_enable;    // auto wander when not docked
    bool     foot_ok;
    bool     touch_ok;
    uint8_t  cliff_mask;
    uint16_t cliff_ch[4];
    bool     petting;
    char     mode[24];          // "idle" | "petting" | "cliff" | "docked" | "explore"
} behavior_status_t;

esp_err_t behavior_start(void);
void behavior_get_status(behavior_status_t *out);

// From K210 UART 0x3F (called on RX task — keep light).
void behavior_on_yolo(uint8_t cls, uint8_t x, uint8_t y);
uint32_t behavior_yolo_count(void);   // every K210 0x3F frame, for the hands-up game

// Runtime toggles (also persisted via settings when saved from web).
void behavior_set_docked(bool on);
bool behavior_docked(void);

// Pause wander/idle faces while Xiaozhi is listening/speaking.
void behavior_set_voice_busy(bool busy);
bool behavior_voice_busy(void);

#ifdef __cplusplus
}
#endif
