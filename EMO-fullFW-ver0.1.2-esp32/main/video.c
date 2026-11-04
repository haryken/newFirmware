// video.c — see video.h.
#include "video.h"
#include "k210_link.h"
#include "k210_uart.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "video";

// PLAY_ANIM playlist JSON (handoff 4.3).
static void anim_json(char *json, size_t n, const char *name, int loop, bool with_mp3)
{
    if (with_mp3) snprintf(json, n, "[%d,[[\"/spiffs/avi/%s.avi\",[[0,\"/spiffs/mp3/%s_000.mp3\"]]]]]", loop, name, name);
    else          snprintf(json, n, "[%d,[[\"/spiffs/avi/%s.avi\"]]]", loop, name);
}

void video_boot_face(const char *name, bool with_mp3)
{
#if !CONFIG_EMO_SD_FACES
    (void)name; (void)with_mp3;
    k210_link_set_anim_done();               // nothing to wait for
    return;
#endif
    char json[200];
    anim_json(json, sizeof(json), name, 0, with_mp3);
    k210_link_clear_anim_done();
    k210_uart_send_short(0x09, 0x00, 0x00, 0x00);
    k210_link_wait_anim_done(200);
    k210_link_clear_ack();
    k210_uart_send_ext(0x02, json);
    bool ok = k210_link_wait_ack(80);
    ESP_LOGI(TAG, "anim %s: %s", name, ok ? "ACK" : "no ACK");
}

esp_err_t video_play_face(const char *name, int loop, bool with_mp3, bool *acked)
{
#if !CONFIG_EMO_SD_FACES
    (void)loop; (void)with_mp3; if (acked) *acked = false;
    return name ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_ARG;   // faces live on the K210 SD card
#endif
    if (k210_link_booting()) return ESP_ERR_INVALID_STATE;
    if (!name || !*name || strlen(name) > 64) return ESP_ERR_INVALID_ARG;
    for (const char *c = name; *c; c++)
        if (*c == '"' || *c == '\\' || *c == '/' || (unsigned char)*c < 0x20) return ESP_ERR_INVALID_ARG;
    char json[200];
    anim_json(json, sizeof(json), name, loop ? 1 : 0, with_mp3);
    k210_link_clear_anim_done();
    k210_uart_send_short(0x09, 0x00, 0x00, 0x00);    // STOP_ANIM
    k210_link_wait_anim_done(200);                   // ANIM_DONE (25-50 ms)
    return k210_send_ext(0x02, json, acked);
}

esp_err_t video_play_playlist(const char *avi, const char *mp3, bool *acked)
{
    if (k210_link_booting()) return ESP_ERR_INVALID_STATE;
    if (!avi || !mp3 || strlen(avi) > 60 || strlen(mp3) > 100) return ESP_ERR_INVALID_ARG;
    char json[256];
    snprintf(json, sizeof(json), "[0,[[\"/spiffs/avi/%s.avi\",[[0,\"/spiffs/mp3/%s.mp3\"]]]]]", avi, mp3);
    k210_link_clear_anim_done();
    k210_uart_send_short(0x09, 0x00, 0x00, 0x00);    // STOP_ANIM
    k210_link_wait_anim_done(200);
    return k210_send_ext(0x02, json, acked);
}

esp_err_t video_show_text(const char *text, bool *acked)
{
    return k210_send_ext(0x44, text, acked);
}

bool video_boot_text(const char *text)
{
    k210_link_clear_ack();
    if (!k210_uart_send_ext(0x44, text)) return false;
    return k210_link_wait_ack(500);
}
