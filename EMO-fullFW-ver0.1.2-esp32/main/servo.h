// servo.h — the four leg servos as PWM outputs.
//
// Pins (emo-libre/newFirmware Hardware/GPIO_MAP.md), 50 Hz, pulse 500 us (0 deg)
// .. 2500 us (180 deg), i.e. the stock angle->pulse of FUN_4025a4b8:
//   0 left legs   GPIO21      1 right legs  GPIO25
//   2 left foot   GPIO5       3 right foot  GPIO26
// The RMT "smart servo" packets on the same pins are the stock calibration /
// firmware-update channel, not the motion path.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERVO_COUNT 4

// LEDC timer + channels; holds 90 deg from boot when EMO_SERVO_HOLD_AT_BOOT.
esp_err_t servo_init(void);

void  servo_write(int i, float deg);      // 0..180, clamps; powers the output
float servo_pos(int i);                   // last commanded angle
void  servo_power_on(void);               // re-drive the last angles after a release
void  servo_release(void);                // duty 0: servos go limp
bool  servo_powered(void);
int   servo_gpio(int i);

#ifdef __cplusplus
}
#endif
