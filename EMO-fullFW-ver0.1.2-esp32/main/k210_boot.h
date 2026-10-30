// k210_boot.h — the stock K210 bring-up sequence (FIRMWARE_HANDOFF.md §7).
//
//   bring-up: RESETN pulse, 2 s, 34 (boot gate), 69 01 every 210 ms until the
//             K210 answers VERSION (<= 30 s), 1.2 s, [2C], 2D 70 38 06 17 6A 6C 74
//   then:     the ESP32's IP on the text screen (waits for Wi-Fi, <= 20 s), 5 s,
//             a forced K210 reset and a second bring-up, and only then
//             boot_start face (+ sound), 65 EB, stay_wink_1, stay_a_b, MODE_IDLE.
// Runs in its own task; k210_link_booting() is true until it finishes.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t k210_boot_start(void);   // spawns the task
bool      k210_boot_done(void);

#ifdef __cplusplus
}
#endif
