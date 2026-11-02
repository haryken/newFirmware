// touch_pet.c — see touch_pet.h.
#include "touch_pet.h"
#include <string.h>
#include "driver/touch_pad.h"
#include "esp_log.h"

static const char *TAG = "touch";
#define THR  0x96
static bool s_ok;
static const touch_pad_t PADS[3] = { TOUCH_PAD_NUM7, TOUCH_PAD_NUM8, TOUCH_PAD_NUM9 };

esp_err_t touch_pet_start(void)
{
    esp_err_t e = touch_pad_init();
    if (e != ESP_OK) { ESP_LOGE(TAG, "init: %s", esp_err_to_name(e)); return e; }
    touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V);
    for (int i = 0; i < 3; i++) {
        e = touch_pad_config(PADS[i], 0);
        if (e != ESP_OK) { ESP_LOGE(TAG, "pad %d: %s", (int)PADS[i], esp_err_to_name(e)); return e; }
    }
    touch_pad_filter_start(10);
    s_ok = true;
    ESP_LOGI(TAG, "pads 7/8/9 thr %u", THR);
    return ESP_OK;
}

bool touch_pet_available(void) { return s_ok; }

esp_err_t touch_pet_poll(touch_pet_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!s_ok) return ESP_ERR_INVALID_STATE;
    out->ok = true;
    for (int i = 0; i < 3; i++) {
        uint16_t v = 0;
        if (touch_pad_read_filtered(PADS[i], &v) != ESP_OK)
            touch_pad_read(PADS[i], &v);
        out->raw[i] = v;
        if (v > 0 && v < THR) out->touched = true;
    }
    return ESP_OK;
}
