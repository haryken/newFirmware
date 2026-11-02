// foot_cliff.h — stock FOOT I2C cliff sensors (addr 0x40 / 0x41 on IMU bus).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     ok;            // FOOT_ID probe succeeded
    uint16_t ch[4];         // filtered channels (stock obj+0x10..)
    uint8_t  mask;          // bit i set when ch[i] looks like a cliff
    uint16_t thr;           // (unused, kept for API)
    uint16_t thr_ch[4];     // per-channel calibrated threshold (0 = not calibrated)
    bool     calibrated;
} foot_cliff_t;

// Stock calibrate: thr = 30% of the current (floor) reading. Call only on the ground.
esp_err_t foot_cliff_calibrate(void);

// Init I2C (GPIO0 SDA / GPIO19 SCL) and probe FOOT IDs. Safe no-op on failure.
esp_err_t foot_cliff_start(void);

// Sample + update mask. Returns ESP_ERR_INVALID_STATE if probe failed.
esp_err_t foot_cliff_poll(foot_cliff_t *out);

bool foot_cliff_available(void);

#ifdef __cplusplus
}
#endif
