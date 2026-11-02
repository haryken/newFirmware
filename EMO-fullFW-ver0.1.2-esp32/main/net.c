// net.c — Wi-Fi STA with open SoftAP + captive DNS for first-time config.
#include "net.h"
#include "settings.h"
#include "captive_dns.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

static const char *TAG = "net";
static EventGroupHandle_t s_eg;
#define GOT_IP   BIT0
static bool s_connected;
static bool s_softap;
static esp_netif_t *s_sta, *s_ap;
static char s_ip[16] = "0.0.0.0";

static portMUX_TYPE s_scan_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_scan_busy;
static char *s_scan_json;   // last completed result, guarded by s_scan_mux

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_softap) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (!s_softap) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip, sizeof(s_ip));
        s_connected = true;
        xEventGroupSetBits(s_eg, GOT_IP);
        ESP_LOGI(TAG, "got IP %s", s_ip);
    }
}

void net_start(void)
{
    s_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL);

    emo_settings_t st; settings_get_copy(&st);

    if (st.wifi_ssid[0]) {
        s_softap = false;
        wifi_config_t sta = {0};
        strncpy((char *)sta.sta.ssid, st.wifi_ssid, sizeof(sta.sta.ssid) - 1);
        strncpy((char *)sta.sta.password, st.wifi_pass, sizeof(sta.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        ESP_LOGI(TAG, "STA mode, connecting to '%s'", st.wifi_ssid);
    } else {
        // APSTA so the captive portal can scan nearby SSIDs while the phone is on our AP.
        s_softap = true;
        wifi_config_t ap = {0};
        strncpy((char *)ap.ap.ssid, CONFIG_EMO_AP_SSID, sizeof(ap.ap.ssid) - 1);
        ap.ap.ssid_len = strlen(CONFIG_EMO_AP_SSID);
        ap.ap.password[0] = 0;
        ap.ap.max_connection = 4;
        ap.ap.authmode = WIFI_AUTH_OPEN;
        ap.ap.channel = 1;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
        strcpy(s_ip, "192.168.4.1");
        ESP_LOGW(TAG, "no SSID saved -> SoftAP '%s' APSTA @ 192.168.4.1 (scan+config)", CONFIG_EMO_AP_SSID);
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    if (s_softap) {
        esp_netif_ip_info_t info;
        esp_ip4_addr_t gw;
        if (esp_netif_get_ip_info(s_ap, &info) == ESP_OK)
            gw = info.ip;
        else
            gw.addr = ESP_IP4TOADDR(192, 168, 4, 1);
        captive_dns_start(gw);
    }
}

bool net_is_connected(void) { return s_connected; }
bool net_is_softap(void) { return s_softap; }
void net_get_ip(char *out, int cap) { strncpy(out, s_ip, cap - 1); out[cap-1] = '\0'; }

static char *build_scan_json(wifi_ap_record_t *recs, uint16_t got)
{
    size_t cap = 64 + (size_t)got * 80;
    char *json = malloc(cap);
    if (!json) return NULL;
    size_t off = 0;
    json[off++] = '[';
    for (uint16_t i = 0; i < got; i++) {
        if (!recs[i].ssid[0]) continue;
        bool dup = false;
        for (uint16_t j = 0; j < i; j++) {
            if (recs[j].ssid[0] && strcmp((char *)recs[j].ssid, (char *)recs[i].ssid) == 0) {
                dup = true; break;
            }
        }
        if (dup) continue;

        char esc[64];
        size_t ei = 0;
        for (const uint8_t *p = recs[i].ssid; *p && ei + 2 < sizeof(esc); p++) {
            char c = (char)*p;
            if (c == '"' || c == '\\') { esc[ei++] = '\\'; esc[ei++] = c; }
            else if ((uint8_t)c < 0x20) esc[ei++] = '?';
            else esc[ei++] = c;
        }
        esc[ei] = 0;

        int nwr = snprintf(json + off, cap - off, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                           off > 1 ? "," : "", esc, (int)recs[i].rssi, (int)recs[i].authmode);
        if (nwr < 0 || (size_t)nwr >= cap - off) break;
        off += (size_t)nwr;
    }
    if (off + 1 < cap) { json[off++] = ']'; json[off] = 0; }
    else { free(json); return strdup("[]"); }
    return json;
}

static void scan_task(void *arg)
{
    (void)arg;
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_scan_config_t sc = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 80, .scan_time.active.max = 200,
    };
    char *json = NULL;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n > 32) n = 32;
        wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(*recs));
        if (recs) {
            uint16_t got = n;
            esp_wifi_scan_get_ap_records(&got, recs);
            json = build_scan_json(recs, got);
            ESP_LOGI(TAG, "wifi scan: %u APs", (unsigned)got);
            free(recs);
        }
    } else {
        ESP_LOGW(TAG, "wifi scan failed");
    }
    if (!json) json = strdup("[]");

    portENTER_CRITICAL(&s_scan_mux);
    free(s_scan_json);
    s_scan_json = json;
    s_scan_busy = false;
    portEXIT_CRITICAL(&s_scan_mux);
    vTaskDelete(NULL);
}

esp_err_t net_wifi_scan_start(void)
{
    portENTER_CRITICAL(&s_scan_mux);
    if (s_scan_busy) {
        portEXIT_CRITICAL(&s_scan_mux);
        return ESP_ERR_INVALID_STATE;
    }
    s_scan_busy = true;
    portEXIT_CRITICAL(&s_scan_mux);

    if (xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 5, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_scan_mux);
        s_scan_busy = false;
        portEXIT_CRITICAL(&s_scan_mux);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

char *net_wifi_scan_json(void)
{
    bool busy;
    char *copy = NULL;
    portENTER_CRITICAL(&s_scan_mux);
    busy = s_scan_busy;
    if (!busy && s_scan_json) copy = strdup(s_scan_json);
    portEXIT_CRITICAL(&s_scan_mux);

    if (busy) return strdup("{\"scanning\":true}");
    if (copy) return copy;
    // No results yet — kick a scan and tell the UI to poll.
    net_wifi_scan_start();
    return strdup("{\"scanning\":true}");
}
