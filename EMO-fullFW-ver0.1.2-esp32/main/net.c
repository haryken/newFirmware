// net.c — Wi-Fi STA with AP fallback.
#include "net.h"
#include "settings.h"
#include "sdkconfig.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

static const char *TAG = "net";
static EventGroupHandle_t s_eg;
#define GOT_IP   BIT0
static bool s_connected;
static esp_netif_t *s_sta, *s_ap;
static char s_ip[16] = "0.0.0.0";

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        esp_wifi_connect();       // keep retrying
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
        // Station only — no fallback SoftAP while an SSID is configured.
        wifi_config_t sta = {0};
        strncpy((char *)sta.sta.ssid, st.wifi_ssid, sizeof(sta.sta.ssid) - 1);
        strncpy((char *)sta.sta.password, st.wifi_pass, sizeof(sta.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        ESP_LOGI(TAG, "STA mode, connecting to '%s'", st.wifi_ssid);
    } else {
        wifi_config_t ap = {0};
        strncpy((char *)ap.ap.ssid, CONFIG_EMO_AP_SSID, sizeof(ap.ap.ssid) - 1);
        strncpy((char *)ap.ap.password, CONFIG_EMO_AP_PASS, sizeof(ap.ap.password) - 1);
        ap.ap.ssid_len = strlen(CONFIG_EMO_AP_SSID);
        ap.ap.max_connection = 4;
        ap.ap.authmode = strlen(CONFIG_EMO_AP_PASS) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
        strcpy(s_ip, "192.168.4.1");
        ESP_LOGW(TAG, "no SSID saved -> SoftAP '%s' at 192.168.4.1", CONFIG_EMO_AP_SSID);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool net_is_connected(void) { return s_connected; }
void net_get_ip(char *out, int cap) { strncpy(out, s_ip, cap - 1); out[cap-1] = '\0'; }
