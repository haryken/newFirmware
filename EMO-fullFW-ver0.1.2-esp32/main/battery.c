// battery.c — see battery.h for the stock measurement path this reproduces.
#include "battery.h"
#include "sdkconfig.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "bat";

#define PIN_ADC      CONFIG_EMO_BAT_ADC_GPIO     // 39 = ADC1_CH3 (stock)
#define PIN_SENSE    CONFIG_EMO_BAT_SENSE_GPIO   // 36 = "GPIO 0x24" (stock)
#define SAMPLES      64                          // stock averages 64 reads
#define PERIOD_MS    2000
#define LOW_MV       3299                        // stock low_power1 threshold

// Stock battery_l1..l4 upper bounds (mV); l5 above l4.
#if CONFIG_EMO_BAT_OLD_SERVO_THRESHOLDS
static const uint16_t TIER_MV[4] = { 3629, 3689, 3769, 4099 };
#else
static const uint16_t TIER_MV[4] = { 3529, 3589, 3669, 3999 };
#endif
#define FULL_SPAN_MV 150                         // l4..100 % spread (meter only)
#define EMPTY_MV     3300                        // 0 % (meter only)

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static battery_t s_bat;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t tier_of(uint16_t mv)
{
    for (int i = 0; i < 4; i++)
        if (mv <= TIER_MV[i]) return (uint8_t)(i + 1);
    return 5;
}

// Smooth 0..100 % for the meter: 20 % per stock tier, linear inside each.
static uint8_t percent_of(uint16_t mv)
{
    uint16_t lo = EMPTY_MV, hi;
    int base = 0;
    for (int i = 0; i < 4; i++, base += 20) {
        hi = TIER_MV[i];
        if (mv <= hi) goto interp;
        lo = hi;
    }
    hi = (uint16_t)(TIER_MV[3] + FULL_SPAN_MV);
    if (mv >= hi) return 100;
interp:
    if (mv <= lo) return (uint8_t)base;
    return (uint8_t)(base + (20 * (mv - lo)) / (hi - lo));
}

static void sample(void)
{
    int sum = 0, raw = 0, n = 0;
    for (int i = 0; i < SAMPLES; i++)
        if (adc_oneshot_read(s_adc, s_chan, &raw) == ESP_OK) { sum += raw; n++; }
    if (!n) return;
    int avg = sum / n, pin_mv = 0;
    if (!s_cali || adc_cali_raw_to_voltage(s_cali, avg, &pin_mv) != ESP_OK)
        pin_mv = avg * 3900 / 4095;              // uncalibrated 11 dB estimate
    bool sense = gpio_get_level(PIN_SENSE);
    int mv = sense ? 2 * pin_mv - 130 : 2 * pin_mv + 50;   // stock FUN_40128b7c
    if (mv < 0) mv = 0;

    battery_t b = {
        .valid = true, .mv = (uint16_t)mv, .pin_mv = (uint16_t)pin_mv, .raw = (uint16_t)avg,
        .percent = percent_of((uint16_t)mv), .tier = tier_of((uint16_t)mv),
        .sense = sense, .low = mv < LOW_MV,
    };
    portENTER_CRITICAL(&s_mux);
    s_bat = b;
    portEXIT_CRITICAL(&s_mux);
}

static void bat_task(void *arg)
{
    int n = 0;
    for (;;) {
        sample();
        if (n++ % 30 == 0)                       // once a minute
            ESP_LOGI(TAG, "%u mV (pin %u mV, raw %u, sense %d) tier %u %u%%%s",
                     s_bat.mv, s_bat.pin_mv, s_bat.raw, s_bat.sense, s_bat.tier,
                     s_bat.percent, s_bat.low ? " LOW" : "");
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
    }
}

void battery_get(battery_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_bat;
    portEXIT_CRITICAL(&s_mux);
}

void battery_start(void)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(PIN_ADC, &unit, &s_chan) != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC1 pin; battery meter disabled", PIN_ADC);
        return;
    }
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    esp_err_t e = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (e != ESP_OK) { ESP_LOGE(TAG, "adc init: %s", esp_err_to_name(e)); return; }
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_chan, &ccfg));

    adc_cali_line_fitting_config_t lcfg = {
        .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
        .default_vref = 1100,
    };
    if (adc_cali_create_scheme_line_fitting(&lcfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration, using a nominal scale");
        s_cali = NULL;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_SENSE, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    sample();
    ESP_LOGI(TAG, "ADC1 ch%d (GPIO%d), sense GPIO%d: %u mV, %u%%",
             (int)s_chan, PIN_ADC, PIN_SENSE, s_bat.mv, s_bat.percent);
    xTaskCreate(bat_task, "battery", 3072, NULL, 3, NULL);
}
