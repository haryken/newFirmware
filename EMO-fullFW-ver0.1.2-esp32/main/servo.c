// servo.c — leg servos, board variant auto-detected like the stock firmware.
//
// Two board variants (stock emo_ota0 boot detect @0x4012dc20, see SERVO.md):
//   OLD servo: supply enable GPIO21, left-legs signal GPIO0   (plain 50 Hz PWM)
//   NEW servo: supply enable GPIO0,  left-legs signal GPIO21  (answers RMT 0xDD)
// Right legs / left foot / right foot are GPIO25 / 5 / 26 on both.
// Detection: power the NEW config, ask each pin for the version (cmd 0xDD); if
// a servo answers it is NEW, otherwise OLD. Overridable (NVS "servo"/"board").
//
// RMT packet FUN_4026171c / FUN_40261698:
//   [0]=0xFF  [1]=cmd  [2]=0x05  [3]=data>>8  [4]=data  [5]=(cmd+hi+lo+4)&0xFF
//   header 256/100 µs, bit1 144/100, bit0 72/100, LSB first, idle LOW
#include "servo.h"
#include "sdkconfig.h"
#include "driver/mcpwm.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "servo";

#define PULSE_MIN_US  500
#define PULSE_SPAN_US 2000
#define FREQ_HZ       50
// OLD board (Kconfig defaults) and NEW board (swapped GPIO0 <-> GPIO21).
#define OLD_PWR_PIN   CONFIG_EMO_SERVO_PWR_GPIO   // 21
#define OLD_LL_PIN    CONFIG_EMO_SERVO_LL_GPIO    // 0
#define NEW_PWR_PIN   OLD_LL_PIN
#define NEW_LL_PIN    OLD_PWR_PIN
#define VERSION_CMD   0xDD                        // stock "read servos version"
#define VERSION_DATA  0x6358
#define NEW_BOOT_MS   2500                        // stock waits this long after powering

#define HDR_HIGH_US   256
#define HDR_LOW_US    100
#define ONE_HIGH_US   144
#define ZERO_HIGH_US  72
#define BIT_LOW_US    100
#define ITEMS_PER_PKT 49

// Runtime pins: [0] and s_pwr_pin depend on the detected board.
static int PINS[SERVO_COUNT] = {
    OLD_LL_PIN, CONFIG_EMO_SERVO_RL_GPIO,
    CONFIG_EMO_SERVO_LF_GPIO, CONFIG_EMO_SERVO_RF_GPIO,
};
static int s_pwr_pin = OLD_PWR_PIN;
static int s_board;                 // 0 unknown, 1 old, 2 new
static int s_mode;                  // 0 auto, 1 old, 2 new (NVS override)
#define PWR_PIN s_pwr_pin
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
        gpio_set_direction(PINS[i], GPIO_MODE_OUTPUT);
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

// ---- board detection ------------------------------------------------------
static rmt_symbol_word_t s_rx_buf[128];

static bool IRAM_ATTR rx_done_cb(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *ed, void *u)
{
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)u, ed, &hp);
    return hp == pdTRUE;
}

// TX one packet on `pin`, release the line, and decode a 6-byte answer.
// RX is armed first with io_loop_back so our own 49 symbols are captured too
// (the answer starts at symbol 49: header, then 48 bits, LSB first, high
// > ~108 us = 1). Returns true if the answer's byte[1] equals `cmd`.
static bool query_pin(int pin, uint8_t cmd, uint16_t data, uint8_t reply[6], int *nsym)
{
    bool ok = false;
    rmt_channel_handle_t rx = NULL, tx = NULL;
    QueueHandle_t q = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (!q) return false;
    if (!s_copy_enc) {
        rmt_copy_encoder_config_t ec = {};
        if (rmt_new_copy_encoder(&ec, &s_copy_enc) != ESP_OK) goto out;
    }

    rmt_rx_channel_config_t rc = {
        .gpio_num = pin, .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000, .mem_block_symbols = 128,
    };
    if (rmt_new_rx_channel(&rc, &rx) != ESP_OK) goto out;
    rmt_rx_event_callbacks_t cbs = { .on_recv_done = rx_done_cb };
    rmt_rx_register_event_callbacks(rx, &cbs, q);
    rmt_enable(rx);

    rmt_tx_channel_config_t tc = {
        .gpio_num = pin, .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000, .mem_block_symbols = 64, .trans_queue_depth = 2,
        .flags = { .io_loop_back = true },
    };
    if (rmt_new_tx_channel(&tc, &tx) != ESP_OK) goto out;
    rmt_enable(tx);

    rmt_receive_config_t rcv = { .signal_range_min_ns = 3000, .signal_range_max_ns = 20000000 };
    if (rmt_receive(rx, s_rx_buf, sizeof(s_rx_buf), &rcv) != ESP_OK) goto out;

    rmt_symbol_word_t items[ITEMS_PER_PKT];
    build_pkt(items, cmd, data);
    rmt_transmit_config_t tcfg = { .loop_count = 0, .flags = { .eot_level = 0 } };
    if (rmt_transmit(tx, s_copy_enc, items, sizeof(items), &tcfg) != ESP_OK) goto out;
    rmt_tx_wait_all_done(tx, pdMS_TO_TICKS(80));
    gpio_set_direction(pin, GPIO_MODE_INPUT);          // let the servo answer
    gpio_set_pull_mode(pin, GPIO_FLOATING);

    rmt_rx_done_event_data_t ev;
    if (xQueueReceive(q, &ev, pdMS_TO_TICKS(300)) == pdTRUE) {
        if (nsym) *nsym = (int)ev.num_symbols;
        if (ev.num_symbols >= ITEMS_PER_PKT * 2) {
            memset(reply, 0, 6);
            for (int b = 0; b < 48; b++) {
                if (ev.received_symbols[ITEMS_PER_PKT + 1 + b].duration0 > 108)
                    reply[b / 8] |= (uint8_t)(1u << (b % 8));
            }
            ok = (reply[1] == cmd);
        }
    }
out:
    if (tx) { rmt_disable(tx); rmt_del_channel(tx); }
    if (rx) { rmt_disable(rx); rmt_del_channel(rx); }
    vQueueDelete(q);
    return ok;
}

// Stock detect (0x4012dc20..): GPIO21 low, supply on GPIO0 high, wait 2.5 s,
// then ask each servo pin for its version. Any answer => NEW servo.
static bool detect_new_servo(void)
{
    static const int probe_pins[SERVO_COUNT] = {
        NEW_LL_PIN, CONFIG_EMO_SERVO_RL_GPIO, CONFIG_EMO_SERVO_LF_GPIO, CONFIG_EMO_SERVO_RF_GPIO,
    };
    gpio_reset_pin(NEW_LL_PIN);
    gpio_set_direction(NEW_LL_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(NEW_LL_PIN, 0);
    gpio_reset_pin(NEW_PWR_PIN);
    gpio_set_direction(NEW_PWR_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(NEW_PWR_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(NEW_PWR_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(NEW_BOOT_MS));

    bool found = false;
    for (int i = 0; i < SERVO_COUNT && !found; i++) {
        for (int t = 0; t < 2 && !found; t++) {
            uint8_t r[6] = {0};
            int n = 0;
            bool ok = query_pin(probe_pins[i], VERSION_CMD, VERSION_DATA, r, &n);
            ESP_LOGI(TAG, "detect gpio%d: %d symbols, reply %02X %02X %02X %02X %02X %02X %s",
                     probe_pins[i], n, r[0], r[1], r[2], r[3], r[4], r[5], ok ? "ANSWER" : "-");
            found = ok;
        }
    }
    return found;
}

static int load_mode(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("servo", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "board", &v);
        nvs_close(h);
    }
    return v <= 2 ? v : 0;
}

static void apply_board(int board)
{
    s_board = board;
    if (board == 2) { PINS[0] = NEW_LL_PIN; s_pwr_pin = NEW_PWR_PIN; }
    else            { PINS[0] = OLD_LL_PIN; s_pwr_pin = OLD_PWR_PIN; }
}

static void arm_new_servos(void)
{
    // Stock power-up (FUN_401f9b54): supply pin LOW, 200 ms, MCPWM init + signal
    // routing, supply pin HIGH, 200 ms. NEW boards are already powered (and
    // booted) by the detection, so they only need the signal routing.
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool already = (s_board == 2) && (s_mode == 0);
    if (!already) {
        gpio_reset_pin(PWR_PIN);
        gpio_set_direction(PWR_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(PWR_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    bind_mcpwm();
    if (!already) {
        gpio_set_level(PWR_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(s_board == 2 ? NEW_BOOT_MS : 200));
    } else {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    xSemaphoreGive(s_mu);
    s_armed = true;
    snprintf(s_last, sizeof(s_last), "%s board, supply gpio%d, pwm %d/%d/%d/%d",
             s_board == 2 ? "NEW" : "OLD", PWR_PIN, PINS[0], PINS[1], PINS[2], PINS[3]);
    ESP_LOGW(TAG, "%s", s_last);
}

int servo_board(void) { return s_board; }
const char *servo_board_name(void) { return s_board == 2 ? "new" : s_board == 1 ? "old" : "unknown"; }
int servo_board_mode(void) { return s_mode; }
int servo_pwr_gpio(void) { return s_pwr_pin; }

esp_err_t servo_set_board_mode(int mode)
{
    if (mode < 0 || mode > 2) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t e = nvs_open("servo", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_u8(h, "board", (uint8_t)mode);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

// Re-route the PWM signals (e.g. after another driver borrowed a shared pin).
void servo_rebind(void)
{
    bind_mcpwm();
    for (int i = 0; i < SERVO_COUNT; i++) drive(i, s_pos[i]);
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

    s_mode = load_mode();
    int board = s_mode;
    if (board == 0) {
        bool is_new = detect_new_servo();
        board = is_new ? 2 : 1;
        ESP_LOGW(TAG, "board auto-detect: %s servo", is_new ? "NEW" : "OLD");
    } else {
        ESP_LOGW(TAG, "board forced by NVS: %s servo", board == 2 ? "NEW" : "OLD");
    }
    apply_board(board);
    if (board == 1) {                        // leave the probe pins as outputs for PWM
        gpio_reset_pin(NEW_PWR_PIN);
        gpio_set_level(NEW_PWR_PIN, 0);
    }

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
