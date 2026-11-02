// http_server.h — the HTTP server: serves the web page (main/web/) and hosts
// the REST API (rest_api.c).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void http_server_start(void);
void http_portal_start(void);   // extra :80 captive portal when the hotspot opens as a fallback
#ifdef __cplusplus
}
#endif
