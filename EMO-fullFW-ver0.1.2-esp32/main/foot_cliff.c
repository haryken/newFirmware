// foot_cliff.c - see foot_cliff.h. Protocol from emo_ota0.elf foot_sensor_*.
#include "foot_cliff.h"
#include <string.h>
#include "driver/i2c_master.h"
#include "sdkconfig.h"
#include "servo.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "foot";

#define I2C_SDA          19
#define I2C_SCL          18
#define I2C_HZ           100000
#define ADDR_A           0x40
#define ADDR_B           0x41
#define THR_DEFAULT      50          // stock seed / sensor_power_cliff_tick low band
#define EMA_NUM          4           // ~stock 0x50/0x80 blend, simplified
#define EMA_DEN          5

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev_a, s_dev_b;
static bool s_ok;
static uint16_t s_ch[4] = { 0x07FF, 0x07FF, 0x07FF, 0x07FF };
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t rd(i2c_master_dev_handle_t dev, uint8_t *buf, size_t n)
{
    return i2c_master_receive(dev, buf, n, 50);
}

static bool probe_ids(void)
{
    uint8_t a = 0, b = 0;
    for (int i = 0; i < 5; i++) {
        if (rd(s_dev_a, &a, 1) == ESP_OK && rd(s_dev_b, &b, 1) == ESP_OK && a == '@' && b == 'A') {
            ESP_LOGI(TAG, "FOOT_ID OK");
            return true;
        }
        ESP_LOGW(TAG, "FOOT_ID fail %02x %02x", a, b);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

esp_err_t foot_cliff_start(void)
{
    // Found with a bit-bang bus scan on an OLD-servo robot: the sensor bus is
    // SDA=19 / SCL=18 (foot 0x40/0x41, ToF 0x29, IMU 0x6a). GPIO0 is the left-legs
    // PWM there, so it must stay out of this. On the NEW-servo board the community
    // map has SDA=0 / SCL=19, but GPIO0 is the servo supply there: not touched.
    if (servo_board() == 2) {
        s_ok = false;
        ESP_LOGW(TAG, "new-servo board: sensor bus pins unverified (GPIO0 is the supply), cliff disabled");
        return ESP_ERR_NOT_SUPPORTED;
    }
    i2c_master_bus_config_t bcfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&bcfg, &s_bus);
    if (e != ESP_OK) { ESP_LOGE(TAG, "i2c bus: %s", esp_err_to_name(e)); return e; }

    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADDR_A,
        .scl_speed_hz = I2C_HZ,
    };
    e = i2c_master_bus_add_device(s_bus, &dcfg, &s_dev_a);
    if (e != ESP_OK) return e;
    dcfg.device_address = ADDR_B;
    e = i2c_master_bus_add_device(s_bus, &dcfg, &s_dev_b);
    if (e != ESP_OK) return e;

    s_ok = probe_ids();
    if (!s_ok) {
        i2c_master_bus_rm_device(s_dev_a);
        i2c_master_bus_rm_device(s_dev_b);
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        ESP_LOGW(TAG, "foot MCUs not found on SDA%d/SCL%d, cliff disabled", I2C_SDA, I2C_SCL);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

bool foot_cliff_available(void) { return s_ok; }

// Stock foot_cliff_calibrate (0x4012ad00): per channel, when the reading is > 50,
// thr = 30 % of it (65 % when it is above 1200), clear level = thr + 20.
// foot_cliff_update_mask (0x4012bb50): bit i set while ch[i] < thr[i].
static uint16_t s_thr[4], s_clr[4];
static bool s_cal;
static uint8_t s_mask;
static uint16_t s_prev[4];
static int s_stable;       // consecutive polls with all channels steady

esp_err_t foot_cliff_calibrate(void)
{
    if (!s_ok || s_stable < 20) return ESP_ERR_INVALID_STATE;   // wait for the filter to settle
    portENTER_CRITICAL(&s_mux);
    int n = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t v = s_ch[i];
        if (v <= 50 || v >= 0x7FF) continue;   // not settled / sensor looks blind
        s_thr[i] = (uint16_t)(v <= 1200 ? v * 3 / 10 : v * 65 / 100);
        s_clr[i] = s_thr[i] + 20;
        n++;
    }
    s_cal = (n == 4);
    s_mask = 0;
    portEXIT_CRITICAL(&s_mux);
    if (s_cal)
        ESP_LOGI(TAG, "calibrated: thr %u %u %u %u", s_thr[0], s_thr[1], s_thr[2], s_thr[3]);
    return s_cal ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t foot_cliff_poll(foot_cliff_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->thr = THR_DEFAULT;
    if (!s_ok) return ESP_ERR_INVALID_STATE;

    uint8_t ba[5] = {0}, bb[5] = {0};
    if (rd(s_dev_a, ba, 5) != ESP_OK || rd(s_dev_b, bb, 5) != ESP_OK)
        return ESP_FAIL;

    // Stock layout after two 5-byte reads: A->[id][u16][u16], B->[id][u16][u16]
    uint16_t raw[4] = {
        (uint16_t)ba[1] | ((uint16_t)ba[2] << 8),
        (uint16_t)ba[3] | ((uint16_t)ba[4] << 8),
        (uint16_t)bb[1] | ((uint16_t)bb[2] << 8),
        (uint16_t)bb[3] | ((uint16_t)bb[4] << 8),
    };

    portENTER_CRITICAL(&s_mux);
    bool steady = true;
    for (int i = 0; i < 4; i++) {
        uint32_t v = ((uint32_t)s_ch[i] * EMA_NUM + (uint32_t)raw[i] * (EMA_DEN - EMA_NUM)) / EMA_DEN;
        if (v > 0x7FF) v = 0x7FF;
        s_ch[i] = (uint16_t)v;
        out->ch[i] = s_ch[i];
        int d = (int)s_ch[i] - (int)s_prev[i];
        if (d > 15 || d < -15) steady = false;
        s_prev[i] = s_ch[i];
        if (s_cal) {
            if (s_ch[i] < s_thr[i]) s_mask |= (uint8_t)(1u << i);
            else if (s_ch[i] >= s_clr[i]) s_mask &= (uint8_t)~(1u << i);
        }
        out->thr_ch[i] = s_cal ? s_thr[i] : 0;
    }
    s_stable = steady ? s_stable + 1 : 0;
    out->mask = s_mask;
    portEXIT_CRITICAL(&s_mux);
    out->calibrated = s_cal;
    out->ok = true;
    return ESP_OK;
}