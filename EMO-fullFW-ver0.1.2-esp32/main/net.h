// net.h — Wi-Fi station with SoftAP fallback for first-time config.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void net_start(void);            // connects to saved SSID, else starts SoftAP
bool net_is_connected(void);
void net_get_ip(char *out, int cap);   // station IP, or AP IP in fallback mode
#ifdef __cplusplus
}
#endif
