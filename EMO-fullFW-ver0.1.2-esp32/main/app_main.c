// app_main.c — brings the modules up and wires their callbacks. No logic here.
//
//   mic path   K210 --SPI--> microphone -> host_stream (TCP to the voice server)
//                                        -> rest_api mic.wav tap
//   play path  rest_api /api/play_pcm, /api/tone -> speaker --SPI--> K210
//   legs       legs (.mot player, gait) -> servo PWM; paired faces -> video
//   screen     video (faces, text, IP banner)
//   web        http_server (main/web/) + rest_api (/api/*)
#include "settings.h"
#include "net.h"
#include "k210_link.h"
#include "k210_boot.h"
#include "microphone.h"
#include "speaker.h"
#include "video.h"
#include "host_stream.h"
#include "http_server.h"
#include "battery.h"
#include "servo.h"
#include "legs.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app";

// Mic PCM from the K210 (16 kHz mono int16). Reaches the host only during an
// utterance (host_stream gates on the session), the web meter/WAV always.
static void mic_sink(const int16_t *pcm, size_t samples)
{
    host_stream_feed(pcm, samples);
}

// Wake word -> open a connection to the host and stream; end of speech -> flush
// and close. One TCP connection per utterance.
static void on_mic_event(microphone_event_t ev)
{
    if (ev == MIC_EV_WAKE) host_stream_session_begin();
    else host_stream_session_end();
}

// A motion starts: show its face animation (+ music) on the K210, as the stock
// behaviour engine does for every profile.json animation (mot + media).
static void on_motion_media(const char *name, bool with_mp3)
{
    bool ack = false;
    esp_err_t e = video_play_face(name, 0, with_mp3, &ack);
    if (e != ESP_OK || !ack) ESP_LOGW(TAG, "motion media %s: %s", name, e != ESP_OK ? esp_err_to_name(e) : "no ACK");
}

void app_main(void)
{
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_init();

    host_stream_init();
    battery_start();
    servo_init();
    legs_set_media_cb(on_motion_media);
    legs_init();
    net_start();                       // Wi-Fi STA (+AP fallback)
    http_server_start();               // web page + REST API

    // K210: mic/speaker modules first (the link RX task calls into them), then
    // the link, then the boot sequence. Wi-Fi + web stay up if the link can't start.
    microphone_init(mic_sink);
    microphone_set_event_cb(on_mic_event);
    speaker_init();
    esp_err_t kerr = k210_link_start();
    if (kerr == ESP_OK) {
        k210_boot_start();             // shows the IP, resets the K210, then the first face
    } else {
        ESP_LOGE(TAG, "K210 link not started: %s", esp_err_to_name(kerr));
    }
    ESP_LOGI(TAG, "EMO-Bridge up. Wake-word utterances stream to the voice server.");

    for (;;) {                         // heartbeat
        vTaskDelay(pdMS_TO_TICKS(10000));
        k210_stats_t k; k210_get_stats(&k);
        char ip[16]; net_get_ip(ip, sizeof(ip));
        ESP_LOGI(TAG, "link=%d state=%u rx=%u mic=%u ip=%s heap=%u (min %u) psram=%u", k.linked, k.state,
                 (unsigned)k.rx_frames, (unsigned)k.mic_samples, ip,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
