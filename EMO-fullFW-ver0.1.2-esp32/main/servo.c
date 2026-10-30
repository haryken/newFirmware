// servo.c — see servo.h.
#include "servo.h"
#include "sdkconfig.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "servo";

#define PULSE_MIN_US  500                // 0 deg
#define PULSE_SPAN_US 2000               // 0..180 deg
#define PERIOD_US     20000              // 50 Hz
#define DUTY_MAX      65535              // LEDC_TIMER_16_BIT

static const int PINS[SERVO_COUNT] = {
    CONFIG_EMO_SERVO_LL_GPIO, CONFIG_EMO_SERVO_RL_GPIO,
    CONFIG_EMO_SERVO_LF_GPIO, CONFIG_EMO_SERVO_RF_GPIO,
};

static float s_pos[SERVO_COUNT] = { 90, 90, 90, 90 };
static bool s_powered;

static uint32_t duty_for(float deg)
{
    if (deg < 0) deg = 0;
    if (deg > 180) deg = 180;
    uint32_t pulse = (uint32_t)(PULSE_MIN_US + deg * PULSE_SPAN_US / 180.0f + 0.5f);   // stock FUN_4025a4b8
    return (uint32_t)((uint64_t)pulse * DUTY_MAX / PERIOD_US);
}

static void drive(int i, float deg)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, duty_for(deg));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i);
}

void servo_write(int i, float deg)
{
    if (i < 0 || i >= SERVO_COUNT) return;
    if (!s_powered) servo_power_on();
    s_pos[i] = deg;
    drive(i, deg);
}

float servo_pos(int i) { return (i >= 0 && i < SERVO_COUNT) ? s_pos[i] : 90.0f; }
bool  servo_powered(void) { return s_powered; }
int   servo_gpio(int i) { return (i >= 0 && i < SERVO_COUNT) ? PINS[i] : -1; }

void servo_power_on(void)
{
    if (s_powered) return;
    s_powered = true;
    for (int i = 0; i < SERVO_COUNT; i++) drive(i, s_pos[i]);
}

void servo_release(void)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i);
    }
    s_powered = false;
}

esp_err_t servo_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_16_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 50, .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t e = ledc_timer_config(&tcfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "ledc timer: %s", esp_err_to_name(e)); return e; }
    for (int i = 0; i < SERVO_COUNT; i++) {
        ledc_channel_config_t c = {
            .gpio_num = PINS[i], .speed_mode = LEDC_LOW_SPEED_MODE, .channel = (ledc_channel_t)i,
            .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0,
        };
        e = ledc_channel_config(&c);
        if (e != ESP_OK) { ESP_LOGE(TAG, "ledc ch%d gpio%d: %s", i, PINS[i], esp_err_to_name(e)); return e; }
    }
#if CONFIG_EMO_SERVO_HOLD_AT_BOOT
    servo_power_on();                             // stock: neutral pose at init
#endif
    ESP_LOGI(TAG, "LL=%d RL=%d LF=%d RF=%d, 50 Hz, %s", PINS[0], PINS[1], PINS[2], PINS[3],
             s_powered ? "holding 90 deg" : "released");
    return ESP_OK;
}
