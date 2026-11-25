// bringup_dns.c - captive-portal DNS: while the hotspot is up, answer every A query with the
// hotspot's address so a phone's connectivity check lands on the setup page.

#include "bringup_priv.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "bringup_dns";

#define AP_IP_BYTES 192, 168, 4, 1

static TaskHandle_t s_task;
static volatile bool s_run;
static int s_sock = -1;

static void dns_task(void *arg)
{
    uint8_t buf[512];
    while (s_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 12 + 5 || n > (int)sizeof(buf) - 16) {
            continue;
        }
        // Header: id(2) flags(2) qd(2) an(2) ns(2) ar(2). Answer only standard queries.
        if ((buf[2] & 0x80) || ((buf[2] >> 3) & 0x0F) != 0 || buf[4] != 0 || buf[5] != 1) {
            continue;
        }
        int q = 12;
        while (q < n && buf[q] != 0) {
            q += buf[q] + 1;
        }
        if (q + 5 > n) {
            continue;
        }
        uint16_t qtype = (buf[q + 1] << 8) | buf[q + 2];
        int end = q + 5; // root label + qtype + qclass
        buf[2] = 0x84 | (buf[2] & 0x01); // response, authoritative, keep RD
        buf[3] = 0x00;                   // no error
        buf[10] = buf[11] = 0;           // no additional records (EDNS dropped)
        buf[8] = buf[9] = 0;
        if (qtype != 1) {                // not A: empty answer
            buf[6] = buf[7] = 0;
            sendto(s_sock, buf, end, 0, (struct sockaddr *)&from, fl);
            continue;
        }
        buf[6] = 0;
        buf[7] = 1;
        static const uint8_t ans[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, AP_IP_BYTES };
        memcpy(&buf[end], ans, sizeof(ans));
        sendto(s_sock, buf, end + sizeof(ans), 0, (struct sockaddr *)&from, fl);
    }
    close(s_sock);
    s_sock = -1;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t bu_dns_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        return ESP_FAIL;
    }
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (bind(s_sock, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }
    s_run = true;
    if (xTaskCreate(dns_task, "bu_dns", 3072, NULL, 4, &s_task) != pdPASS) {
        s_run = false;
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "captive DNS up");
    return ESP_OK;
}

void bu_dns_stop(void)
{
    s_run = false; // the task exits within its 1 s receive timeout
}
