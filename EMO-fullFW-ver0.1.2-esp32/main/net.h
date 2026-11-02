// net.h — Wi-Fi station with SoftAP fallback for first-time config.
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
void net_start(void);            // connects to saved SSID, else starts SoftAP
bool net_is_connected(void);
bool net_is_softap(void);        // true = provisioning SoftAP (no saved SSID)
void net_get_ip(char *out, int cap);   // station IP, or AP IP in fallback mode

// Non-blocking Wi-Fi scan for the captive portal.
// net_wifi_scan_start(): kick a background scan (ESP_ERR_INVALID_STATE if busy).
// net_wifi_scan_json(): malloc'd JSON — either AP list, {"scanning":true}, or "[]".
char *net_wifi_scan_json(void);
esp_err_t net_wifi_scan_start(void);

#ifdef __cplusplus
}
#endif
