// rest_api.h — every /api/* endpoint (documented in API.md).
//
//   GET/POST /api/config     settings (Wi-Fi)              settings.c
//   GET      /api/status     link / mic / speaker / host / battery statistics
//   POST     /api/tone       0.5 s 440 Hz test tone         speaker.c
//   POST     /api/play_pcm   raw 48 kHz mono int16 LE body  speaker.c
//   GET      /api/mic.wav    3 s mic capture as WAV         microphone.c + host_stream tap
//   GET/POST /api/controls   named K210 commands            k210_controls.c
//   GET/POST /api/motion     leg motion                     legs.c
//   POST     /api/power      {"action":"off"}: whole robot off  k210_link.c
#pragma once
#include "esp_http_server.h"
#ifdef __cplusplus
extern "C" {
#endif
void rest_api_register(httpd_handle_t srv);
#ifdef __cplusplus
}
#endif
