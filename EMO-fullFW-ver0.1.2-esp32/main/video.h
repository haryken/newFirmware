// video.h — the robot screen: face animations, text, the IP banner.
//
// Faces (FIRMWARE_HANDOFF.md 4.3): STOP_ANIM (09) -> ANIM_DONE -> PLAY_ANIM, an
// extended frame 02 with the playlist JSON
//   [loop,[["/spiffs/avi/NAME.avi"(,[[0,"/spiffs/mp3/NAME_000.mp3"]])]]]
// The K210 reads every .avi/.mp3 from its own SD card; menuconfig EMO_SD_FACES
// turns all of this off while the card is broken.
// Text: extended frame 44 (the stock factory-test text screen).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Face animation. Name = asset without path/extension (letters, digits, _ -).
// loop 0 = once (K210 sends ANIM_DONE after), 1 = until replaced. with_mp3 adds
// the paired sound. Waits <= 500 ms for the ACK, reported in *acked.
// ESP_ERR_NOT_SUPPORTED when EMO_SD_FACES is off, ESP_ERR_INVALID_STATE while booting.
esp_err_t video_play_face(const char *name, int loop, bool with_mp3, bool *acked);

// Boot-sequence variant: no validation, 200 ms ANIM_DONE wait, 80 ms ACK wait.
void video_boot_face(const char *name, bool with_mp3);

// Text on the screen (ext 44). Waits <= 500 ms for the ACK.
esp_err_t video_show_text(const char *text, bool *acked);

// Boot-sequence variant of the text screen (allowed while booting). Returns the ACK.
bool video_boot_text(const char *text);

#ifdef __cplusplus
}
#endif
