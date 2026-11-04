// k210_file.c - see k210_file.h.
#include "k210_file.h"
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "k210_spi.h"
#include "k210_uart.h"
#include "k210_link.h"

static const char *TAG = "k210_file";
#define CHUNK_MAX 10240

static SemaphoreHandle_t s_lock;
static uint8_t *s_dma;
static uint8_t *s_dst;
static size_t s_cap, s_len;
static uint32_t s_chunks;
static volatile bool s_active, s_done;

void k210_file_on_chunk(uint16_t n)
{
    if (!s_active || !s_dma) return;
    if (n > CHUNK_MAX) n = CHUNK_MAX;
    esp_rom_delay_us(500);
    k210_spi_read(s_dma, n, 2, 600);
    k210_swap4(s_dma, (n + 3) & ~3u);
    size_t c = n;
    if (s_len + c > s_cap) c = s_cap - s_len;
    memcpy(s_dst + s_len, s_dma, c);
    s_len += c;
    s_chunks++;
}

void k210_file_on_done(void) { s_done = true; }

esp_err_t k210_file_read(const char *path, uint8_t *dst, size_t cap, size_t *len, int timeout_ms)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_dma = heap_caps_malloc(CHUNK_MAX + 64, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_dma) { xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM; }
    s_dst = dst; s_cap = cap; s_len = 0; s_chunks = 0; s_done = false; s_active = true;
    k210_uart_send_short(0x2C, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(60));
    bool ack = false;
    k210_send_ext(0x03, path, &ack);
    int waited = 0;
    while (!s_done && waited < timeout_ms) { vTaskDelay(pdMS_TO_TICKS(20)); waited += 20; }
    vTaskDelay(pdMS_TO_TICKS(50));
    s_active = false;
    k210_uart_send_short(0x2D, 0, 0, 0);
    ESP_LOGI(TAG, "%s: %u bytes in %u chunks (ack=%d done=%d)", path, (unsigned)s_len, (unsigned)s_chunks, (int)ack, (int)s_done);
    if (len) *len = s_len;
    free(s_dma); s_dma = NULL;
    xSemaphoreGive(s_lock);
    return s_len ? ESP_OK : ESP_ERR_NOT_FOUND;
}
