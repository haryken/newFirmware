// anim.h - play a stock "animation" (body .mot + K210 face/sound) by profile name.
// Data comes from /spiffs/anim_tbl.txt, generated from the stock profile.json:
//   A|<anim>|<mot|->|<media1;media2;...>      animation -> motion + random pick of media
//   M|<media>|<avi>|<mp3|->|<seconds>         media -> face clip, song, length
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start animation (or bare media) `name`. *dur_ms = length of its media (0 if unknown).
// Returns ESP_ERR_NOT_FOUND if the name is in neither table.
esp_err_t anim_play(const char *name, uint32_t *dur_ms);

#ifdef __cplusplus
}
#endif
