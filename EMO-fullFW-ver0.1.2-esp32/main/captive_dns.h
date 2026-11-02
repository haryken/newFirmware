// captive_dns.h — tiny DNS that points every query at the SoftAP gateway (captive portal).
#pragma once
#include <stdbool.h>
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

void captive_dns_start(esp_ip4_addr_t gateway);
void captive_dns_stop(void);
bool captive_dns_running(void);

#ifdef __cplusplus
}
#endif
