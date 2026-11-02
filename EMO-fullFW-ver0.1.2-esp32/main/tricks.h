// tricks.h — sure-bet stock-like routines (camping / timer / gated explore / game demos).
// No home-station, camera pipeline, or BT — see BEHAVIOR_BLOCKED.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     mode[24];       // idle|camping|timer|explore|game
    char     detail[40];     // e.g. game name / seconds left
    bool     busy;
    uint32_t timer_ms_left;
    uint32_t explore_ms_left;
} tricks_status_t;

void tricks_init(void);
void tricks_get_status(tricks_status_t *out);

// Stop any running trick (not leg motion from MCP play_motion unless owned by tricks).
void tricks_stop(void);

esp_err_t tricks_camping_start(void);
esp_err_t tricks_timer_start(uint32_t ms);     // 1 s .. 30 min
void      tricks_timer_cancel(void);

esp_err_t tricks_explore_start(uint32_t seconds);  // 5..120; needs cliff; blocked if docked
void      tricks_explore_stop(void);

// game: "rps" | "ttt" | "zombie" | "fix_bugs"
esp_err_t tricks_game_start(const char *game);

#ifdef __cplusplus
}
#endif
