// bringup_wifi.c - station mode with the saved credentials, the WPA2 setup hotspot, the
// fallback hotspot after repeated failed connects, and the network scan for the setup page.

#include "bringup_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bringup_wifi";

static esp_netif_t *s_sta, *s_ap;
static bool s_have_creds;
static volatile bool s_connected, s_scanning, s_ap_up, s_bringup;
static int s_fails;
static char s_ip[16] = "";
static char s_ap_ssid[33] = "";
static esp_timer_handle_t s_retry;

// Copy a C string into a fixed Wi-Fi config field that need not be NUL-terminated when full.
static void put_field(uint8_t *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    memset(dst, 0, cap);
    memcpy(dst, src, n < cap ? n : cap);
}

static void ap_up(void)
{
    if (s_ap_up) {
        return;
    }
    wifi_config_t ap = { 0 };
    put_field(ap.ap.ssid, sizeof(ap.ap.ssid), s_ap_ssid);
    ap.ap.ssid_len = strlen(s_ap_ssid) < sizeof(ap.ap.ssid) ? strlen(s_ap_ssid) : sizeof(ap.ap.ssid);
    put_field(ap.ap.password, sizeof(ap.ap.password), g_bu_cfg->ap_password);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 4;
    ap.ap.channel = 1;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    s_ap_up = true;
    bu_dns_start();
    ESP_LOGW(TAG, "hotspot \"%s\" up at 192.168.4.1/setup", s_ap_ssid);
}

static void ap_down(void)
{
    if (!s_ap_up) {
        return;
    }
    bu_dns_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_up = false;
    ESP_LOGI(TAG, "fallback hotspot down");
}

static void retry_cb(void *arg)
{
    if (s_have_creds && !s_connected && !s_scanning) {
        esp_wifi_connect();
    }
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (s_have_creds) {
            esp_wifi_connect();
        }
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = data;
        bool was = s_connected;
        s_connected = false;
        s_ip[0] = '\0';
        if (s_scanning || !s_have_creds) {
            break;
        }
        s_fails++;
        uint8_t limit = g_bu_cfg->sta_fail_limit ? g_bu_cfg->sta_fail_limit : 5;
        ESP_LOGW(TAG, "wifi %s (reason %d), attempt %d", was ? "lost" : "connect failed",
                 d->reason, s_fails);
        if (s_fails >= limit && !s_ap_up) {
            ESP_LOGW(TAG, "%d failed connects: starting the fallback hotspot", s_fails);
            ap_up();
        }
        // Back off a little once failing, so the hotspot stays usable for a phone.
        esp_timer_start_once(s_retry, (s_fails < limit ? 2 : 10) * 1000000ULL);
        break;
    }
    default:
        break;
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *ev = data;
    if (ev->esp_netif != s_sta) {
        return;
    }
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    s_connected = true;
    s_fails = 0;
    ESP_LOGI(TAG, "wifi connected, ip %s", s_ip);
    if (!s_bringup) {
        ap_down();
    }
}

esp_err_t bu_wifi_start(bool bringup)
{
    s_bringup = bringup;
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X",
             g_bu_cfg->ap_ssid_prefix ? g_bu_cfg->ap_ssid_prefix : "Setup", mac[4], mac[5]);
    if (g_bu_cfg->hostname_prefix) {
        char host[32];
        snprintf(host, sizeof(host), "%s-%02x%02x", g_bu_cfg->hostname_prefix, mac[4], mac[5]);
        esp_netif_set_hostname(s_sta, host);
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_wifi_set_storage(WIFI_STORAGE_RAM); // credentials live in the bring-up namespace only
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL));
    const esp_timer_create_args_t ta = { .callback = retry_cb, .name = "bu_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_retry));

    wifi_config_t sta = { 0 };
    char ssid[33], pass[65];
    s_have_creds = bu_read(BRINGUP_KEY_WIFI_SSID, ssid, sizeof(ssid));
    if (!bu_read(BRINGUP_KEY_WIFI_PASS, pass, sizeof(pass))) {
        pass[0] = '\0';
    }
    if (s_have_creds) {
        put_field(sta.sta.ssid, sizeof(sta.sta.ssid), ssid);
        put_field(sta.sta.password, sizeof(sta.sta.password), pass);
        sta.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    }
    memset(pass, 0, sizeof(pass));

    esp_wifi_set_mode(bringup ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    memset(&sta, 0, sizeof(sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (bringup) {
        ap_up();
    }
    ESP_LOGI(TAG, "wifi started: %s%s%s", bringup ? "bring-up hotspot" : "station",
             s_have_creds ? ", joining " : ", no saved network", s_have_creds ? ssid : "");
    return ESP_OK;
}

bool bu_wifi_sta_connected(void) { return s_connected; }
bool bringup_hotspot_up(void) { return s_ap_up; }
const char *bu_wifi_ip(void) { return s_ip; }
const char *bu_wifi_ap_ssid(void) { return s_ap_ssid; }

bool bu_wifi_scan_json(char *buf, size_t len)
{
    s_scanning = true;
    if (!s_connected) {
        esp_wifi_disconnect(); // a pending connect attempt blocks the scan
    }
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    uint16_t n = 0;
    wifi_ap_record_t *recs = NULL;
    if (e == ESP_OK) {
        esp_wifi_scan_get_ap_num(&n);
        if (n > 24) {
            n = 24;
        }
        recs = calloc(n ? n : 1, sizeof(*recs));
        if (recs && esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
            n = 0;
        }
    }
    s_scanning = false;
    if (s_have_creds && !s_connected) {
        esp_wifi_connect();
    }
    if (e != ESP_OK || !recs) {
        free(recs);
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(e));
        return false;
    }
    // Records come sorted by RSSI; drop duplicate SSIDs and hidden networks.
    size_t o = snprintf(buf, len, "[");
    int out = 0;
    for (uint16_t i = 0; i < n && o < len; i++) {
        const char *ss = (const char *)recs[i].ssid;
        bool dup = ss[0] == '\0';
        for (uint16_t j = 0; j < i && !dup; j++) {
            dup = strcmp(ss, (const char *)recs[j].ssid) == 0;
        }
        if (dup) {
            continue;
        }
        char esc[70];
        size_t k = 0;
        for (const char *p = ss; *p && k < sizeof(esc) - 2; p++) {
            if (*p == '"' || *p == '\\') {
                esc[k++] = '\\';
            }
            esc[k++] = (unsigned char)*p < 0x20 ? '?' : *p;
        }
        esc[k] = '\0';
        o += snprintf(buf + o, len - o, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                      out++ ? "," : "", esc, recs[i].rssi,
                      recs[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
    }
    if (o < len) {
        snprintf(buf + o, len - o, "]");
    }
    free(recs);
    return o + 1 < len;
}
