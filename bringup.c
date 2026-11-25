// bringup.c - settings store, validation and the bring-up mode decision.

#include "bringup_priv.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bringup";

const bringup_config_t *g_bu_cfg;
const bringup_field_t *g_bu_fields[BRINGUP_MAX_FIELDS];
size_t g_bu_nfields;

static bool s_active;
static char s_reason[48] = "";
static esp_timer_handle_t s_pc_timer;

// Reserved keys in the project's namespace (start with '_' so they cannot clash with fields).
#define KEY_FORCE "_force"   // set by bringup_enter, cleared at the next boot
#define KEY_PCNT  "_pcycles" // quick power-cycle counter

static const bringup_field_t s_builtin[] = {
    { .key = BRINGUP_KEY_WIFI_SSID, .label = "Wi-Fi network", .group = "Wi-Fi",
      .type = BRINGUP_T_STRING, .required = true, .max = 32 },
    { .key = BRINGUP_KEY_WIFI_PASS, .label = "Wi-Fi password", .group = "Wi-Fi",
      .help = "Leave empty for an open network", .type = BRINGUP_T_STRING, .secret = true,
      .max = 63 },
    { .key = BRINGUP_KEY_NAME, .label = "Device name", .group = "Device",
      .type = BRINGUP_T_STRING, .max = 24 },
    { .key = BRINGUP_KEY_ADMIN_PASS, .label = "Admin password", .group = "Device",
      .help = "Protects this page, OTA and the device's API (user \"admin\"), 8 or more characters",
      .type = BRINGUP_T_STRING, .secret = true, .required = true, .min = 8, .max = 63 },
};

const bringup_field_t *bu_field(const char *key)
{
    for (size_t i = 0; i < g_bu_nfields; i++) {
        if (strcmp(g_bu_fields[i]->key, key) == 0) {
            return g_bu_fields[i];
        }
    }
    return NULL;
}

// ---- NVS ----

bool bu_read(const char *key, char *out, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(g_bu_cfg->nvs_namespace, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = len;
    esp_err_t e = nvs_get_str(h, key, out, &n);
    nvs_close(h);
    return e == ESP_OK && out[0] != '\0';
}

esp_err_t bu_write(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(g_bu_cfg->nvs_namespace, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    e = (value && value[0]) ? nvs_set_str(h, key, value) : nvs_erase_key(h, key);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        e = ESP_OK;
    }
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e;
}

static uint8_t sys_get_u8(const char *key)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(g_bu_cfg->nvs_namespace, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v;
}

static void sys_set_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(g_bu_cfg->nvs_namespace, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_u8(h, key, v);
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "NVS write %s failed: %s", key, esp_err_to_name(e));
    }
}

// ---- validation ----

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool parse_mac(const char *s, uint8_t out[6])
{
    for (int i = 0; i < 6; i++) {
        int hi = hexval(s[0]), lo = hi >= 0 ? hexval(s[1]) : -1;
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
        s += 2;
        if (i < 5) {
            if (*s != ':' && *s != '-') {
                return false;
            }
            s++;
        }
    }
    return *s == '\0';
}

static bool parse_hex(const char *s, uint8_t *out, size_t n)
{
    if (strlen(s) != 2 * n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static bool choice_ok(const char *choices, const char *v)
{
    size_t vl = strlen(v);
    const char *p = choices;
    while (p && *p) {
        const char *bar = strchr(p, '|');
        size_t l = bar ? (size_t)(bar - p) : strlen(p);
        if (l == vl && strncmp(p, v, l) == 0) {
            return true;
        }
        p = bar ? bar + 1 : NULL;
    }
    return false;
}

bool bu_validate(const bringup_field_t *f, const char *v, char *err, size_t errlen)
{
    size_t len = strlen(v);
    if (len == 0) {
        if (f->required) {
            snprintf(err, errlen, "required");
            return false;
        }
        return true;
    }
    if (len >= BRINGUP_VAL_MAX) {
        snprintf(err, errlen, "too long");
        return false;
    }
    switch (f->type) {
    case BRINGUP_T_STRING: {
        int32_t max = f->max ? f->max : 63;
        if ((int32_t)len > max || (int32_t)len < f->min) {
            snprintf(err, errlen, "length must be %ld to %ld", (long)f->min, (long)max);
            return false;
        }
        for (size_t i = 0; i < len; i++) {
            if ((unsigned char)v[i] < 0x20 || v[i] == 0x7F) {
                snprintf(err, errlen, "control characters not allowed");
                return false;
            }
        }
        return true;
    }
    case BRINGUP_T_MAC: {
        uint8_t m[6];
        if (!parse_mac(v, m)) {
            snprintf(err, errlen, "use aa:bb:cc:dd:ee:ff");
            return false;
        }
        return true;
    }
    case BRINGUP_T_HEX: {
        uint8_t tmp[64];
        if (f->max <= 0 || f->max > (int32_t)sizeof(tmp) || !parse_hex(v, tmp, (size_t)f->max)) {
            snprintf(err, errlen, "use %ld hex digits", (long)(2 * f->max));
            return false;
        }
        return true;
    }
    case BRINGUP_T_INT: {
        char *end;
        long n = strtol(v, &end, 10);
        if (*end != '\0' || n < f->min || n > f->max) {
            snprintf(err, errlen, "whole number %ld to %ld", (long)f->min, (long)f->max);
            return false;
        }
        return true;
    }
    case BRINGUP_T_BOOL:
        if (strcmp(v, "0") != 0 && strcmp(v, "1") != 0) {
            snprintf(err, errlen, "0 or 1");
            return false;
        }
        return true;
    case BRINGUP_T_CHOICE:
        if (!choice_ok(f->choices, v)) {
            snprintf(err, errlen, "one of %s", f->choices ? f->choices : "");
            return false;
        }
        return true;
    }
    return false;
}

// Normalise what is stored: MAC and hex in lower case with ':' separators.
static void normalise(const bringup_field_t *f, const char *in, char *out, size_t len)
{
    if (f->type == BRINGUP_T_MAC) {
        uint8_t m[6];
        if (parse_mac(in, m)) {
            snprintf(out, len, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
            return;
        }
    }
    snprintf(out, len, "%s", in);
    if (f->type == BRINGUP_T_HEX) {
        for (char *p = out; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
    }
}

// ---- public getters / setters ----

bool bringup_get_str(const char *key, char *out, size_t len)
{
    const bringup_field_t *f = bu_field(key);
    if (!f || !out || len == 0) {
        return false;
    }
    if (bu_read(key, out, len)) {
        return true;
    }
    if (f->def && f->def[0]) {
        snprintf(out, len, "%s", f->def);
        return true;
    }
    out[0] = '\0';
    return false;
}

bool bringup_is_set(const char *key)
{
    char v[BRINGUP_VAL_MAX];
    return bringup_get_str(key, v, sizeof(v));
}

bool bringup_get_mac(const char *key, uint8_t mac[6])
{
    char v[BRINGUP_VAL_MAX];
    return bringup_get_str(key, v, sizeof(v)) && parse_mac(v, mac);
}

bool bringup_get_hex(const char *key, uint8_t *out, size_t n)
{
    char v[BRINGUP_VAL_MAX];
    bool ok = bringup_get_str(key, v, sizeof(v)) && parse_hex(v, out, n);
    memset(v, 0, sizeof(v));
    return ok;
}

int32_t bringup_get_int(const char *key, int32_t def)
{
    char v[BRINGUP_VAL_MAX];
    return bringup_get_str(key, v, sizeof(v)) ? (int32_t)strtol(v, NULL, 10) : def;
}

bool bringup_get_bool(const char *key, bool def)
{
    char v[BRINGUP_VAL_MAX];
    return bringup_get_str(key, v, sizeof(v)) ? strcmp(v, "1") == 0 : def;
}

esp_err_t bringup_set_str(const char *key, const char *value)
{
    const bringup_field_t *f = bu_field(key);
    char err[64], norm[BRINGUP_VAL_MAX];
    if (!f || !value) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!bu_validate(f, value, err, sizeof(err))) {
        ESP_LOGW(TAG, "%s rejected: %s", key, err);
        return ESP_ERR_INVALID_ARG;
    }
    normalise(f, value, norm, sizeof(norm));
    esp_err_t e = bu_write(key, norm);
    memset(norm, 0, sizeof(norm));
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "%s %s", key, f->secret ? "set (secret)" : "set");
    }
    return e;
}

esp_err_t bringup_set_mac(const char *key, const uint8_t m[6])
{
    char v[18];
    snprintf(v, sizeof(v), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return bringup_set_str(key, v);
}

esp_err_t bringup_set_hex(const char *key, const uint8_t *d, size_t n)
{
    char v[BRINGUP_VAL_MAX];
    if (2 * n + 1 > sizeof(v)) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < n; i++) {
        snprintf(&v[2 * i], 3, "%02x", d[i]);
    }
    esp_err_t e = bringup_set_str(key, v);
    memset(v, 0, sizeof(v));
    return e;
}

esp_err_t bringup_seed(const char *key, const char *value)
{
    char cur[BRINGUP_VAL_MAX];
    if (!value || !value[0] || bu_read(key, cur, sizeof(cur))) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "seeding %s", key);
    return bringup_set_str(key, value);
}

// ---- mode ----

bool bringup_active(void) { return s_active; }
const char *bringup_reason(void) { return s_reason; }
bool bringup_wifi_connected(void) { return bu_wifi_sta_connected(); }

void bringup_enter(void)
{
    ESP_LOGW(TAG, "entering bring-up mode on request");
    sys_set_u8(KEY_FORCE, 1);
    vTaskDelay(pdMS_TO_TICKS(300)); // let netserial ship the lines above
    esp_restart();
}

static void pc_window_done(void *arg)
{
    sys_set_u8(KEY_PCNT, 0); // this boot lasted long enough: not part of a quick power-cycle run
}

esp_err_t bringup_init(const bringup_config_t *cfg)
{
    if (!cfg || !cfg->nvs_namespace || !cfg->ap_password || strlen(cfg->ap_password) < 8 ||
        cfg->n_fields + sizeof(s_builtin) / sizeof(s_builtin[0]) > BRINGUP_MAX_FIELDS) {
        return ESP_ERR_INVALID_ARG;
    }
    g_bu_cfg = cfg;
    g_bu_nfields = 0;
    for (size_t i = 0; i < sizeof(s_builtin) / sizeof(s_builtin[0]); i++) {
        g_bu_fields[g_bu_nfields++] = &s_builtin[i];
    }
    for (size_t i = 0; i < cfg->n_fields; i++) {
        g_bu_fields[g_bu_nfields++] = &cfg->fields[i];
    }

    ESP_LOGI(TAG, "reset reason %d", (int)esp_reset_reason());
    if (cfg->on_seed) {
        cfg->on_seed();
    }

    // Quick power cycles: count boots that end before the window; N in a row force bring-up.
    uint8_t limit = cfg->power_cycles ? cfg->power_cycles : 3;
    uint16_t window = cfg->power_cycle_window_s ? cfg->power_cycle_window_s : 10;
    bool by_cycles = false;
    if (limit != 255 && esp_reset_reason() == ESP_RST_POWERON) {
        uint8_t n = sys_get_u8(KEY_PCNT) + 1;
        ESP_LOGI(TAG, "quick power cycle %u of %u", n, limit);
        if (n >= limit) {
            by_cycles = true;
            n = 0;
        }
        sys_set_u8(KEY_PCNT, n);
        const esp_timer_create_args_t ta = { .callback = pc_window_done, .name = "bu_pc" };
        if (esp_timer_create(&ta, &s_pc_timer) == ESP_OK) {
            esp_timer_start_once(s_pc_timer, (uint64_t)window * 1000000ULL);
        }
    } else if (limit != 255) {
        sys_set_u8(KEY_PCNT, 0); // a software or watchdog reset breaks the run
    }

    bool forced = sys_get_u8(KEY_FORCE) != 0;
    if (forced) {
        sys_set_u8(KEY_FORCE, 0);
    }

    const char *missing = NULL;
    for (size_t i = 0; i < g_bu_nfields; i++) {
        if (g_bu_fields[i]->required && !bringup_is_set(g_bu_fields[i]->key)) {
            missing = g_bu_fields[i]->key;
            break;
        }
    }

    if (missing) {
        snprintf(s_reason, sizeof(s_reason), "not configured: %s", missing);
    } else if (forced) {
        snprintf(s_reason, sizeof(s_reason), "requested");
    } else if (by_cycles) {
        snprintf(s_reason, sizeof(s_reason), "power cycles");
    }
    s_active = s_reason[0] != '\0';
    if (s_active) {
        ESP_LOGW(TAG, "bring-up mode (%s)", s_reason);
    }
    return bu_wifi_start(s_active);
}
