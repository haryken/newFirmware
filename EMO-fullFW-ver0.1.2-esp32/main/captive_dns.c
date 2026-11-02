// captive_dns.c — see captive_dns.h. Adapted from 78/esp-wifi-connect DnsServer (Xiaozhi).
#include "captive_dns.h"
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

static const char *TAG = "captive_dns";
static int s_fd = -1;
static volatile bool s_run;
static esp_ip4_addr_t s_gw;
static TaskHandle_t s_task;

static void dns_task(void *arg)
{
    (void)arg;
    char buf[512];
    while (s_run) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int len = recvfrom(s_fd, buf, sizeof(buf), 0, (struct sockaddr *)&client, &clen);
        if (len < 0) {
            if (!s_run) break;
            continue;
        }
        if (len < 12) continue;
        buf[2] |= 0x80;
        buf[3] |= 0x80;
        buf[7] = 1;
        if (len + 16 > (int)sizeof(buf)) continue;
        memcpy(&buf[len], "\xc0\x0c", 2);
        len += 2;
        memcpy(&buf[len], "\x00\x01\x00\x01\x00\x00\x00\x1c\x00\x04", 10);
        len += 10;
        memcpy(&buf[len], &s_gw.addr, 4);
        len += 4;
        sendto(s_fd, buf, len, 0, (struct sockaddr *)&client, clen);
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

void captive_dns_start(esp_ip4_addr_t gateway)
{
    captive_dns_stop();
    s_gw = gateway;
    s_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "socket fail");
        return;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);
    if (bind(s_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind :53 fail errno=%d", errno);
        close(s_fd);
        s_fd = -1;
        return;
    }
    s_run = true;
    xTaskCreate(dns_task, "captive_dns", 3072, NULL, 5, &s_task);
    char ip[16];
    esp_ip4addr_ntoa(&gateway, ip, sizeof(ip));
    ESP_LOGI(TAG, "DNS captive -> %s", ip);
}

void captive_dns_stop(void)
{
    if (!s_run && s_fd < 0) return;
    s_run = false;
    if (s_fd >= 0) {
        shutdown(s_fd, SHUT_RDWR);
        close(s_fd);
        s_fd = -1;
    }
    for (int i = 0; i < 20 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
}

bool captive_dns_running(void) { return s_run; }
