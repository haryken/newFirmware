// http_server.c — see http_server.h.
#include "http_server.h"
#include "rest_api.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "http";
static httpd_handle_t s_srv;

// Page files embedded from main/web/ (CMakeLists EMBED_TXTFILES).
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");
extern const char style_css_start[]  asm("_binary_style_css_start");
extern const char style_css_end[]    asm("_binary_style_css_end");
extern const char app_js_start[]     asm("_binary_app_js_start");
extern const char app_js_end[]       asm("_binary_app_js_end");

static esp_err_t send_static(httpd_req_t *r, const char *type, const char *start, const char *end)
{
    httpd_resp_set_type(r, type);
    return httpd_resp_send(r, start, end - start - 1);   // EMBED_TXTFILES appends a NUL
}
static esp_err_t get_index(httpd_req_t *r) { return send_static(r, "text/html", index_html_start, index_html_end); }
static esp_err_t get_css(httpd_req_t *r)   { return send_static(r, "text/css", style_css_start, style_css_end); }
static esp_err_t get_js(httpd_req_t *r)    { return send_static(r, "application/javascript", app_js_start, app_js_end); }

void http_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 20;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }

    static const httpd_uri_t pages[] = {
        { .uri = "/",          .method = HTTP_GET, .handler = get_index },
        { .uri = "/style.css", .method = HTTP_GET, .handler = get_css },
        { .uri = "/app.js",    .method = HTTP_GET, .handler = get_js },
    };
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        esp_err_t e = httpd_register_uri_handler(s_srv, &pages[i]);
        if (e != ESP_OK) ESP_LOGE(TAG, "register %s failed: %s", pages[i].uri, esp_err_to_name(e));
    }
    rest_api_register(s_srv);
    ESP_LOGI(TAG, "web page + REST API up");
}
