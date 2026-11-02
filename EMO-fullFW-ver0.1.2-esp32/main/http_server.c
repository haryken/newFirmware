// http_server.c — see http_server.h.
#include "http_server.h"
#include "rest_api.h"
#include "net.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "http";
static httpd_handle_t s_srv;

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char style_css_start[]  asm("_binary_style_css_start");
extern const char style_css_end[]    asm("_binary_style_css_end");
extern const char app_js_start[]     asm("_binary_app_js_start");
extern const char app_js_end[]       asm("_binary_app_js_end");

static esp_err_t send_static(httpd_req_t *r, const char *type, const char *start, const char *end)
{
    httpd_resp_set_type(r, type);
    return httpd_resp_send(r, start, end - start - 1);
}
static esp_err_t get_index(httpd_req_t *r) { return send_static(r, "text/html", index_html_start, index_html_end); }
static esp_err_t get_css(httpd_req_t *r)   { return send_static(r, "text/css", style_css_start, style_css_end); }
static esp_err_t get_js(httpd_req_t *r)    { return send_static(r, "application/javascript", app_js_start, app_js_end); }

// SoftAP captive probes (Android/iOS/Windows) → open config UI, Xiaozhi-style.
static esp_err_t captive_redirect(httpd_req_t *r)
{
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(r, "Connection", "close");
    httpd_resp_send(r, NULL, 0);
    return ESP_OK;
}

static httpd_handle_t s_portal;

static void register_pages(httpd_handle_t srv)
{
    static const httpd_uri_t pages[] = {
        { .uri = "/",          .method = HTTP_GET, .handler = get_index },
        { .uri = "/style.css", .method = HTTP_GET, .handler = get_css },
        { .uri = "/app.js",    .method = HTTP_GET, .handler = get_js },
    };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(srv, &pages[i]);
        if (e != ESP_OK) ESP_LOGE(TAG, "register %s failed: %s", pages[i].uri, esp_err_to_name(e));
    }
    rest_api_register(srv);
}

static void register_probes(httpd_handle_t srv)
{
    static const char *probes[] = {
        "/hotspot-detect.html",
        "/generate_204*",
        "/gen_204*",
        "/mobile/status.php",
        "/check_network_status.txt",
        "/ncsi.txt",
        "/fwlink*",
        "/connectivity-check.html",
        "/success.txt",
        "/portal.html",
        "/library/test/success.html",
        "/connecttest.txt",
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        httpd_uri_t u = {
            .uri = probes[i], .method = HTTP_GET, .handler = captive_redirect,
        };
        httpd_register_uri_handler(srv, &u);
    }
}

void http_server_start(void)
{
    const bool ap = net_is_softap();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    // SoftAP: :80 so the phone captive portal pops the Wi-Fi page. STA: :8080 as before.
    cfg.server_port = ap ? 80 : 8080;
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 40;
    cfg.lru_purge_enable = true;
    if (ap) cfg.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }

    register_pages(s_srv);
    if (ap) {
        register_probes(s_srv);
        ESP_LOGI(TAG, "SoftAP captive portal + config UI on :80 (open Wi-Fi, no password)");
    } else {
        ESP_LOGI(TAG, "web page + REST API on :8080");
    }
}

// Saved Wi-Fi failed -> hotspot was opened later (net.c). The :8080 server is
// already up, so add a second one on :80 that answers the captive probes.
void http_portal_start(void)
{
    if (s_portal) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32769;
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 40;
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_portal, &cfg) != ESP_OK) { s_portal = NULL; ESP_LOGE(TAG, "portal httpd start failed"); return; }
    register_pages(s_portal);
    register_probes(s_portal);
    ESP_LOGI(TAG, "captive portal on :80 (hotspot fallback)");
}