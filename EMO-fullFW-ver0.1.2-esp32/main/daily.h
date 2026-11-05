// daily.h - stock "daily life" schedule + editable slots for the web UI.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DAILY_MAX_SLOTS   24
#define DAILY_NAME_LEN    20

typedef struct {
    uint16_t start;                 // HHMM
    uint16_t end;                   // HHMM
    char     a[DAILY_NAME_LEN];     // primary activity (or "*" = special random)
    char     b[DAILY_NAME_LEN];     // optional alternate ("" = none)
    uint8_t  enabled;               // 0/1
} daily_slot_t;

typedef struct {
    bool enabled;
    bool synced;
    int  hhmm;
    int  fake_hhmm;
    char last[28];
    char next[40];
    int  tz_hours;
    int  nslots;
    daily_slot_t slots[DAILY_MAX_SLOTS];
} daily_status_t;

void daily_init(void);
void daily_get_status(daily_status_t *out);
void daily_set_enabled(bool on);
void daily_set_fake_hhmm(int hhmm);
esp_err_t daily_set_tz(int hours);

// Replace the whole schedule (1..DAILY_MAX_SLOTS). Persisted to NVS.
esp_err_t daily_set_schedule(const daily_slot_t *slots, int n);
void daily_reset_schedule(void);            // restore stock defaults

// Catalog of activity names the UI can pick (NULL-terminated).
const char *const *daily_activity_names(void);

#ifdef __cplusplus
}
#endif
