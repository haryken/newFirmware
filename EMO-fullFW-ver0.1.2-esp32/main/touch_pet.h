// touch_pet.h — ESP32 touch pads 7/8/9 (stock thr ~0x96).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     ok;
    bool     touched;       // any pad below threshold
    uint16_t raw[3];        // pads 7,8,9
} touch_pet_t;

esp_err_t touch_pet_start(void);
esp_err_t touch_pet_poll(touch_pet_t *out);
bool touch_pet_available(void);

#ifdef __cplusplus
}
#endif
