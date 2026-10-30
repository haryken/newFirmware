// battery.h — battery voltage / level, as the stock ESP32 firmware measured it.
//
// Source: decompiled stock firmware (power manager FUN_40128db0 / FUN_40128b7c,
// tier mapping FUN_401e5430; see emo-new-firmware/Documentation/
// BATTERY_CHARGING_STARTUP_ANALYSIS.md). emo-libre/newFirmware's GPIO map only
// documents the power-enable pin (GPIO23), not the sense path.
//
//   adc1_config_channel_atten(ADC1_CH3 = GPIO39, 11 dB); 64 raw samples averaged;
//   esp_adc_cal_raw_to_voltage -> mV at the pin;
//   GPIO36 (input, "0x24") selects the divider/offset:  low  -> vbat = 2*mV + 50
//                                                       high -> vbat = 2*mV - 130
//   low-power alert below 3299 mV; tiers battery_l1..l5 (-> 20/40/60/80/100 %):
//     new-servo boards: l1 <= 3529, l2 <= 3589, l3 <= 3669, l4 <= 3999, else l5
//     old-servo boards: l1 <= 3629, l2 <= 3689, l3 <= 3769, l4 <= 4099, else l5
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     valid;     // at least one measurement done
    uint16_t mv;        // battery voltage, mV (stock formula)
    uint16_t pin_mv;    // calibrated voltage at the ADC pin, mV
    uint16_t raw;       // averaged 12-bit raw ADC value
    uint8_t  percent;   // 0..100, interpolated inside the stock tiers
    uint8_t  tier;      // 1..5 = stock battery_l1..battery_l5
    bool     sense;     // level of the GPIO36 sense input (selects the offset)
    bool     low;       // below the stock low-power threshold (3299 mV)
} battery_t;

// Configure ADC + sense GPIO and start the 2 s sampling task.
void battery_start(void);
void battery_get(battery_t *out);

#ifdef __cplusplus
}
#endif
