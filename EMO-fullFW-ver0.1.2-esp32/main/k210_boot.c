// k210_boot.c — see k210_boot.h.
#include "k210_boot.h"
#include "k210_link.h"
#include "k210_uart.h"
#include "microphone.h"
#include "video.h"
#include "net.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "k210_boot";
static volatile bool s_done;

#define IP_WAIT_MS   20000      // how long to wait for a Wi-Fi address before showing the screen anyway
#define IP_SHOW_MS   5000       // how long the IP stays on the screen before the K210 is reset

bool k210_boot_done(void) { return s_done; }

// Reset pulse, boot gate, VERSION poll, stock staging frames: the K210 is then
// alive, idle, and ready for screen / face commands.
static void bring_up(const char *why)
{
    k210_link_clear_version();
    ESP_LOGI(TAG, "%s: reset + settle", why);
    k210_link_reset_pulse(1);
    vTaskDelay(pdMS_TO_TICKS(2000));
    k210_uart_send_short(0x34, 0x00, 0x00, 0x00);          // boot gate

    // GET_VERSION every 210 ms until the K210 answers (stock: 3rd try, ~8 s after power-on)
    ESP_LOGI(TAG, "polling GET_VERSION");
    int64_t t0 = esp_timer_get_time();
    while (!k210_link_version_seen() && esp_timer_get_time() - t0 < 30000000) {
        k210_uart_send_short(0x69, 0x01, 0x00, 0x00);
        vTaskDelay(pdMS_TO_TICKS(210));
    }
    if (k210_link_version_seen()) ESP_LOGI(TAG, "K210 firmware %s", k210_link_version());
    else ESP_LOGW(TAG, "no VERSION reply in 30 s, continuing anyway");
    vTaskDelay(pdMS_TO_TICKS(1200));                       // READY (47) follows ~0.9 s later

#if CONFIG_EMO_BOOT_FILE_SESSION
    k210_uart_send_short(0x2C, 0x00, 0x00, 0x00);          // file session begin (stock; read-only)
    vTaskDelay(pdMS_TO_TICKS(200));
#endif
    // REQ_FILE "/spiffs/json/profile.json" is deliberately skipped: it makes the
    // K210 push FILE_CHUNKs over SPI with a read command we don't know.
    static const uint8_t stage[][4] = {
        { 0x2D, 0x00, 0x00, 0x00 }, { 0x70, 0x00, 0x00, 0x00 }, { 0x38, 0x01, 0x00, 0x00 },
        { 0x06, 0x01, 0x00, 0x00 }, { 0x17, 0x68, 0x00, 0x00 }, { 0x6A, 0x46, 0x00, 0x00 },
        { 0x6C, 0x00, 0x00, 0x00 }, { 0x74, 0x00, 0x00, 0x00 },
    };
    for (size_t i = 0; i < sizeof(stage) / sizeof(stage[0]); i++) {
#if !CONFIG_EMO_BOOT_FILE_SESSION
        if (stage[i][0] == 0x2D) continue;                 // no session was opened
#endif
        k210_uart_send_short(stage[i][0], stage[i][1], stage[i][2], stage[i][3]);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// "EMO-Bridge / IP x.x.x.x" on the stock text screen, waiting for Wi-Fi first.
static void show_ip(void)
{
    char ip[16] = "";
    int64_t t0 = esp_timer_get_time();
    for (;;) {
        net_get_ip(ip, sizeof(ip));
        if (ip[0] && strcmp(ip, "0.0.0.0") != 0) break;
        if (esp_timer_get_time() - t0 > (int64_t)IP_WAIT_MS * 1000) { ip[0] = '\0'; break; }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    char text[48];
    snprintf(text, sizeof(text), ip[0] ? "EMO-Bridge\nIP %s" : "EMO-Bridge\nno Wi-Fi yet", ip);
    bool ack = video_boot_text(text);
    ESP_LOGI(TAG, "IP on screen: %s (%s)", ip[0] ? ip : "none", ack ? "ACK" : "no ACK");
}

static void boot_task(void *arg)
{
    s_done = false;
    microphone_mark_closed();
    k210_link_set_state(LINK_IDLE);

    // 1. first bring-up, just to put the IP on the screen
    bring_up("boot");
    show_ip();
    vTaskDelay(pdMS_TO_TICKS(IP_SHOW_MS));

    // 2. forced K210 reboot, then the real bring-up and the first face
    bring_up("reboot after IP screen");

    video_boot_face("boot_start", true);                   // ACK now, ANIM_DONE ~4 s later
#if CONFIG_EMO_SD_FACES
    k210_link_clear_anim_done();
    k210_link_wait_anim_done(6000);
#endif
    k210_uart_send_short(0x65, 0xEB, 0x00, 0x00);
    vTaskDelay(pdMS_TO_TICKS(70));
    video_boot_face("stay_wink_1", false);
    vTaskDelay(pdMS_TO_TICKS(900));
    video_boot_face("stay_a_b", false);
    vTaskDelay(pdMS_TO_TICKS(1000));
    k210_uart_send_mode(K210_MODE_IDLE);                   // wake word armed
    k210_link_set_state(LINK_IDLE);
    s_done = true;
    ESP_LOGI(TAG, "done, MODE_IDLE (wake word armed)");
    k210_link_set_booting(false);
    vTaskDelete(NULL);
}

esp_err_t k210_boot_start(void)
{
    k210_link_set_booting(true);
    if (xTaskCreate(boot_task, "k210_boot", 5120, NULL, 5, NULL) != pdPASS) {
        k210_link_set_booting(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
