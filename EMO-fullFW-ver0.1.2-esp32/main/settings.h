// settings.h — persistent, web-editable configuration stored in NVS.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// All strings are NUL-terminated. Sizes include the terminator.
typedef struct {
    // Wi-Fi station (applied at the next boot)
    char wifi_ssid[33];
    char wifi_pass[65];
    // Voice server: where each wake-word utterance is streamed. Defaults from
    // menuconfig EMO_HOST_IP / EMO_HOST_PORT; applied at the next utterance.
    char     host_ip[16];
    uint16_t host_port;
    // Default motion speed, percent of the file timing (100 = as authored; applies
    // to every play without an explicit speed). menuconfig EMO_MOTION_SPEED_PCT.
    uint16_t motion_speed_pct;
} emo_settings_t;

// Load from NVS into *out, filling defaults for any missing key. Never fails hard.
void settings_load(emo_settings_t *out);

// Persist *in to NVS. Returns 0 on success.
int settings_save(const emo_settings_t *in);

// Thread-safe copy of the live, in-RAM settings (updated on save).
void settings_get_copy(emo_settings_t *out);

// Apply a single key=value (used by the web REST layer). Returns 0 if the key
// was recognised and the value valid. Does not persist; call settings_save().
int settings_set_kv(emo_settings_t *s, const char *key, const char *val);

// Serialise current settings to a JSON object (secrets redacted when redact=true).
// Returns a malloc'd string the caller must free, or NULL on OOM.
char *settings_to_json(const emo_settings_t *s, bool redact);

void settings_init(void);

#ifdef __cplusplus
}
#endif
