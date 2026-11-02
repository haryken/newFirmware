// servo.c — stock new-servo arm (RMT) then MCPWM 50 Hz gait.
//
// Packet FUN_4026171c / FUN_40261698 (asm, not the broken decompile):
//   [0]=0xFF  [1]=cmd  [2]=0x05  [3]=data>>8  [4]=data  [5]=(cmd+hi+lo+4)&0xFF
//   header 256/100 µs, bit1 144/100, bit0 72/100, LSB first, idle LOW
// SET_MODE at boot: C5/0, C3/0x44, C4/0x44 then mcpwm_set_duty_in_us.
#include "servo.h"
#include "sdkconfig.h"
#include "driver/mcpwm.h"
#include "driver/rmt_tx.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>

static const char *TAG = "servo";

#define PULSE_MIN_US  500
#define PULSE_SPAN_US 2000
#define FREQ_HZ       50

#define HDR_HIGH_US   256
#define HDR_LOW_US    100
#define ONE_HIGH_US   144
#define ZERO_HIGH_US  72
#define BIT_LOW_US    100
#define ITEMS_PER_PKT 49

static const int PINS[SERVO_COUNT] = {
    CONFIG_EMO_SERVO_LL_GPIO, CONFIG_EMO_SERVO_RL_GPIO,
    CONFIG_EMO_SERVO_LF_GPIO, CONFIG_EMO_SERVO_RF_GPIO,
};
static const mcpwm_timer_t TIMERS[SERVO_COUNT] = {
    MCPWM_TIMER_0, MCPWM_TIMER_0, MCPWM_TIMER_1, MCPWM_TIMER_1,
};
static const mcpwm_generator_t GENS[SERVO_COUNT] = {
    MCPWM_GEN_A, MCPWM_GEN_B, MCPWM_GEN_A, MCPWM_GEN_B,
};
static const mcpwm_io_signals_t SIGNALS[SERVO_COUNT] = {
    MCPWM0A, MCPWM0B, MCPWM1A, MCPWM1B,
};

static float s_pos[SERVO_COUNT] = { 90, 90, 90, 90 };
static uint32_t s_pulse[SERVO_COUNT] = { 1500, 1500, 1500, 1500 };
static bool s_powered;
static bool s_mcpwm_ready;
static bool s_armed;
static uint8_t s_rmt_cmd = 0xC5;
static int s_encode;
static int s_ack_ok;
static int s_ack_fail;
static char s_last[96] = "init";
static SemaphoreHandle_t s_mu;
static rmt_encoder_handle_t s_copy_enc;

static uint32_t pulse_us(float deg)
{
    if (deg < 0) deg = 0;
    if (deg > 180) deg = 180;
    return (uint32_t)(PULSE_MIN_US + deg * PULSE_SPAN_US / 180.0f + 0.5f);
}

static void set_last(const char *s)
{
    snprintf(s_last, sizeof(s_last), "%s", s);
}

static void build_pkt(rmt_symbol_word_t *out, uint8_t cmd, uint16_t data)
{
    uint8_t hi = (uint8_t)(data >> 8);
    uint8_t lo = (uint8_t)data;
    uint8_t cs = (uint8_t)(cmd + hi + lo + 4);
    uint8_t bytes[6] = { 0xFF, cmd, 0x05, hi, lo, cs };

    out[0] = (rmt_symbol_word_t){
        .duration0 = HDR_HIGH_US, .level0 = 1,
        .duration1 = HDR_LOW_US,  .level1 = 0,
    };
    int n = 1;
    for (int b = 0; b < 6; b++) {
        uint8_t v = bytes[b];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t h = (v & 1) ? ONE_HIGH_US : ZERO_HIGH_US;
            out[n++] = (rmt_symbol_word_t){
                .duration0 = h, .level0 = 1,
                .duration1 = BIT_LOW_US, .level1 = 0,
            };
            v >>= 1;
        }
    }
}

static esp_err_t rmt_send_pin(int gpio, uint8_t cmd, uint16_t data)
{
    if (!s_copy_enc) {
        rmt_copy_encoder_config_t ec = {};
        ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&ec, &s_copy_enc), TAG, "enc");
    }
    rmt_channel_handle_t tx = NULL;
    rmt_tx_channel_config_t cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
        .flags = { .invert_out = false, .with_dma = false, .io_od_mode = false },
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&cfg, &tx), TAG, "tx");
    ESP_RETURN_ON_ERROR(rmt_enable(tx), TAG, "en");

    rmt_symbol_word_t items[ITEMS_PER_PKT];
    build_pkt(items, cmd, data);
    rmt_transmit_config_t tcfg = {
        .loop_count = 0,
        .flags = { .eot_level = 0 },
    };
    esp_err_t e = rmt_transmit(tx, s_copy_enc, items, sizeof(items), &tcfg);
    if (e == ESP_OK) e = rmt_tx_wait_all_done(tx, pdMS_TO_TICKS(80));
    rmt_disable(tx);
    rmt_del_channel(tx);
    gpio_reset_pin(gpio);
    gpio_set_direction(gpio, GPIO_MODE_INPUT);
    gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
    return e;
}

static void bind_mcpwm(void)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        esp_err_t e = mcpwm_gpio_init(MCPWM_UNIT_0, SIGNALS[i], PINS[i]);
        if (e != ESP_OK) ESP_LOGE(TAG, "mcpwm gpio %d: %s", PINS[i], esp_err_to_name(e));
    }
}

static void drive(int i, float deg)
{
    uint32_t us = pulse_us(deg);
    s_pulse[i] = us;
    mcpwm_set_duty_type(MCPWM_UNIT_0, TIMERS[i], GENS[i], MCPWM_DUTY_MODE_0);
    mcpwm_set_duty_in_us(MCPWM_UNIT_0, TIMERS[i], GENS[i], us);
    mcpwm_start(MCPWM_UNIT_0, TIMERS[i]);
}

static void arm_new_servos(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < SERVO_COUNT; i++) {
        mcpwm_set_duty_in_us(MCPWM_UNIT_0, TIMERS[i], GENS[i], 0);
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    static const struct { uint8_t cmd; uint16_t data; } steps[] = {
        { 0xC5, 0x0000 },
        { 0xC3, 0x0044 },
        { 0xC4, 0x0044 },
    };
    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
        for (int i = 0; i < SERVO_COUNT; i++) {
            uint8_t cs = (uint8_t)(steps[s].cmd + (steps[s].data & 0xFF) + 4);
            esp_err_t e = rmt_send_pin(PINS[i], steps[s].cmd, steps[s].data);
            ESP_LOGI(TAG, "arm gpio%d cmd=0x%02X data=0x%04X cs=0x%02X %s",
                     PINS[i], steps[s].cmd, steps[s].data, cs, esp_err_to_name(e));
            if (e == ESP_OK) s_ack_ok++; else s_ack_fail++;
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    bind_mcpwm();
    xSemaphoreGive(s_mu);
    s_armed = true;
    snprintf(s_last, sizeof(s_last), "arm cs-ok tx_ok=%d fail=%d", s_ack_ok, s_ack_fail);
    ESP_LOGW(TAG, "%s", s_last);
}

void servo_write(int i, float deg)
{
    if (i < 0 || i >= SERVO_COUNT) return;
    if (deg < 0) deg = 0;
    if (deg > 180) deg = 180;
    if (!s_powered) servo_power_on();
    s_pos[i] = deg;
    drive(i, deg);
}

void servo_flush(void)
{
    if (!s_powered) servo_power_on();
    for (int i = 0; i < SERVO_COUNT; i++) drive(i, s_pos[i]);
}

float    servo_pos(int i) { return (i >= 0 && i < SERVO_COUNT) ? s_pos[i] : 90.0f; }
uint32_t servo_pulse_us(int i) { return (i >= 0 && i < SERVO_COUNT) ? s_pulse[i] : 0; }
bool     servo_powered(void) { return s_powered; }
int      servo_gpio(int i) { return (i >= 0 && i < SERVO_COUNT) ? PINS[i] : -1; }
const char *servo_diag(void) { return s_last; }
int servo_ack_ok(void) { return s_ack_ok; }
int servo_ack_fail(void) { return s_ack_fail; }

void servo_set_rmt_cmd(uint8_t cmd) { s_rmt_cmd = cmd; }
uint8_t servo_rmt_cmd(void) { return s_rmt_cmd; }
void servo_set_encode(int mode) { s_encode = mode; (void)s_encode; }

void servo_power_on(void)
{
    if (!s_mcpwm_ready) return;
    if (!s_armed) arm_new_servos();
    else bind_mcpwm();
    s_powered = true;
    for (int i = 0; i < SERVO_COUNT; i++) drive(i, s_pos[i]);
    set_last("mcpwm on");
}

void servo_release(void)
{
    for (int i = 0; i < SERVO_COUNT; i++) {
        mcpwm_set_duty_in_us(MCPWM_UNIT_0, TIMERS[i], GENS[i], 0);
    }
    s_powered = false;
    set_last("released");
}

void servo_bitbang_test(int deg, int ms)
{
    (void)deg;
    (void)ms;
    servo_power_on();
}

void servo_rmt_poke(uint8_t cmd, uint16_t data)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < SERVO_COUNT; i++) {
        mcpwm_set_duty_in_us(MCPWM_UNIT_0, TIMERS[i], GENS[i], 0);
        esp_err_t e = rmt_send_pin(PINS[i], cmd, data);
        ESP_LOGI(TAG, "poke gpio%d cmd=0x%02X data=0x%04X %s",
                 PINS[i], cmd, data, esp_err_to_name(e));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    bind_mcpwm();
    xSemaphoreGive(s_mu);
    snprintf(s_last, sizeof(s_last), "poke 0x%02X data=%u", cmd, (unsigned)data);
    s_powered = true;
    for (int i = 0; i < SERVO_COUNT; i++) drive(i, s_pos[i]);
}

void servo_pin_buzz(int ms)
{
    (void)ms;
    servo_power_on();
}

esp_err_t servo_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    mcpwm_config_t cfg = {
        .frequency = FREQ_HZ,
        .cmpr_a = 0,
        .cmpr_b = 0,
        .duty_mode = MCPWM_DUTY_MODE_0,
        .counter_mode = MCPWM_UP_COUNTER,
    };
    esp_err_t e = mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_0, &cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "mcpwm t0: %s", esp_err_to_name(e)); return e; }
    e = mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_1, &cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "mcpwm t1: %s", esp_err_to_name(e)); return e; }
    s_mcpwm_ready = true;

    arm_new_servos();

#if CONFIG_EMO_SERVO_HOLD_AT_BOOT
    s_powered = true;
    for (int i = 0; i < SERVO_COUNT; i++) drive(i, 90.0f);
    ESP_LOGI(TAG, "MCPWM hold 90 after RMT SET_MODE");
#endif
    ESP_LOGI(TAG, "ready pins %d/%d/%d/%d diag=%s",
             PINS[0], PINS[1], PINS[2], PINS[3], s_last);
    return ESP_OK;
}
