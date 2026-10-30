// k210_spi.c — see k210_spi.h.
#include "k210_spi.h"
#include "sdkconfig.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

static const char *TAG = "k210_spi";

#define SPI_HOSTID  (CONFIG_EMO_SPI_HOST)
#define PIN_SCLK    CONFIG_EMO_SPI_SCLK
#define PIN_MOSI    CONFIG_EMO_SPI_MOSI
#define PIN_MISO    CONFIG_EMO_SPI_MISO
#define PIN_CS      CONFIG_EMO_SPI_CS
#define SPI_HZ      CONFIG_EMO_SPI_HZ

static spi_device_handle_t s_spi;
static SemaphoreHandle_t s_lock;
static uint32_t s_reads, s_writes;
static WORD_ALIGNED_ATTR uint8_t s_hdr[8];   // DMA scratch for the command phase

static inline void cs_low(void)  { gpio_set_level(PIN_CS, 0); }
static inline void cs_high(void) { gpio_set_level(PIN_CS, 1); }

void k210_swap4(uint8_t *p, size_t len)
{
    size_t groups = (len + 3) / 4;
    for (size_t i = 0; i < groups; i++) {
        uint8_t *q = p + i * 4;
        uint8_t t;
        t = q[0]; q[0] = q[3]; q[3] = t;
        t = q[1]; q[1] = q[2]; q[2] = t;
    }
}

static void build_hdr(uint16_t len, uint8_t mode)
{
    uint8_t lo = len & 0xFF, hi = (len >> 8) & 0xFF;
    s_hdr[0] = 0; s_hdr[1] = 0;
    s_hdr[2] = lo; s_hdr[3] = hi;
    s_hdr[4] = mode;
    s_hdr[5] = (uint8_t)(lo + hi + mode);
    s_hdr[6] = 0; s_hdr[7] = 0;
}

static esp_err_t xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .rxlength = rx ? len * 8 : 0,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

void k210_spi_write(uint8_t *wire, uint16_t len, uint8_t mode, uint32_t gap_us)
{
    size_t padded = (len + 63) & ~63u;
    memset(wire + len, 0, padded - len);
    build_hdr(len, mode);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cs_low();  xfer(s_hdr, NULL, sizeof(s_hdr));  cs_high();
    esp_rom_delay_us(gap_us);
    cs_low();  xfer(wire, NULL, padded);  cs_high();
    xSemaphoreGive(s_lock);
    s_writes++;
}

void k210_spi_read(uint8_t *dst, uint16_t len, uint8_t mode, uint32_t gap_us)
{
    size_t padded = (len + 63) & ~63u;
    build_hdr(len, mode);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cs_low();  xfer(s_hdr, NULL, sizeof(s_hdr));  cs_high();
    esp_rom_delay_us(gap_us);
    cs_low();  xfer(NULL, dst, padded);  cs_high();
    xSemaphoreGive(s_lock);
    s_reads++;
}

void k210_spi_counts(uint32_t *reads, uint32_t *writes)
{
    if (reads) *reads = s_reads;
    if (writes) *writes = s_writes;
}

esp_err_t k210_spi_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    gpio_config_t cs = { .pin_bit_mask = 1ULL << PIN_CS, .mode = GPIO_MODE_OUTPUT };
    esp_err_t e = gpio_config(&cs);
    if (e != ESP_OK) return e;
    cs_high();

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = K210_SPI_MAX_XFER + 64,
    };
    if ((e = spi_bus_initialize(SPI_HOSTID, &bus, SPI_DMA_CH_AUTO)) != ESP_OK) return e;
    spi_device_interface_config_t dev = {
        .clock_speed_hz = SPI_HZ,
        .mode = 1,                       // CPOL0/CPHA1
        .duty_cycle_pos = 128,
        .spics_io_num = -1,              // CS driven per phase by hand
        .queue_size = 7,
        .flags = 0,
    };
    if ((e = spi_bus_add_device(SPI_HOSTID, &dev, &s_spi)) != ESP_OK) return e;
    ESP_LOGI(TAG, "SPI%d mode 1 %d Hz, SCLK %d MOSI %d MISO %d CS %d", SPI_HOSTID, SPI_HZ,
             PIN_SCLK, PIN_MOSI, PIN_MISO, PIN_CS);
    return ESP_OK;
}
