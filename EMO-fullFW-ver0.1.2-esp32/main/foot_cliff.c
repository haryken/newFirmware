// foot_cliff.c — see foot_cliff.h. Protocol from emo_ota0.elf foot_sensor_*.
#include "foot_cliff.h"
#include <string.h>
#include "driver/i2c_master.h"
#include "sdkconfig.h"
#include "servo.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "foot";

#define I2C_SDA          0
#define I2C_SCL          19
#define I2C_HZ           100000
#define ADDR_A           0x40
#define ADDR_B           0x41
#define THR_DEFAULT      50          // stock seed / sensor_power_cliff_tick low band
#define EMA_NUM          5           // ~stock 0x50/0x80 blend, simplified
#define EMA_DEN          8

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
    // New-servo board: GPIO0 is the servo supply enable. Putting the I2C driver
    // on it would toggle the supply, so leave the foot sensors alone.
    if (servo_board() == 2) {
        s_ok = false;
        ESP_LOGW(TAG, "new-servo board: GPIO0 is the servo supply, foot I2C skipped, cliff disabled");
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

    bool found = probe_ids();
    // GPIO0 is the foot-I2C SDA but also the left-legs PWM signal (old servo
    // board). The stock firmware also loses the foot MCUs once the PWM owns the
    // pin ("FOOT ID ... fail"). Give the pin back to the servo driver.
    i2c_master_bus_rm_device(s_dev_a);
    i2c_master_bus_rm_device(s_dev_b);
    i2c_del_master_bus(s_bus);
    s_bus = NULL;
    servo_rebind();
    s_ok = false;
    ESP_LOGW(TAG, "foot MCUs %s; I2C released, GPIO0 returned to the servo driver, cliff disabled",
             found ? "seen" : "not seen");
    return ESP_ERR_NOT_SUPPORTED;
}

bool foot_cliff_available(void) { return s_ok; }

esp_err_t foot_cliff_poll(foot_cliff_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->thr = THR_DEFAULT;
    if (!s_ok) return ESP_ERR_INVALID_STATE;

    uint8_t ba[5] = {0}, bb[5] = {0};
    if (rd(s_dev_a, ba, 5) != ESP_OK || rd(s_dev_b, bb, 5) != ESP_OK)
        return ESP_FAIL;

    // Stock layout after two 5-byte reads: A→[id][u16][u16], B→[id][u16][u16]
    uint16_t raw[4] = {
        (uint16_t)ba[1] | ((uint16_t)ba[2] << 8),
        (uint16_t)ba[3] | ((uint16_t)ba[4] << 8),
        (uint16_t)bb[1] | ((uint16_t)bb[2] << 8),
        (uint16_t)bb[3] | ((uint16_t)bb[4] << 8),
    };

    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < 4; i++) {
        uint32_t v = ((uint32_t)s_ch[i] * EMA_NUM + (uint32_t)raw[i] * (EMA_DEN - EMA_NUM)) / EMA_DEN;
        if (v > 0x7FF) v = 0x7FF;
        s_ch[i] = (uint16_t)v;
        out->ch[i] = s_ch[i];
        if (s_ch[i] < THR_DEFAULT) out->mask |= (uint8_t)(1u << i);
    }
    portEXIT_CRITICAL(&s_mux);
    out->ok = true;
    return ESP_OK;
}
