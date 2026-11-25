#pragma once

#include "bringup.h"

// Shared between the component's files only.

#define BRINGUP_MAX_FIELDS 32
#define BRINGUP_VAL_MAX    96 // longest stored value + 1

extern const bringup_config_t *g_bu_cfg;
extern const bringup_field_t *g_bu_fields[BRINGUP_MAX_FIELDS];
extern size_t g_bu_nfields;

const bringup_field_t *bu_field(const char *key);
// Validate value for field f; on failure returns false and writes a short message into err.
bool bu_validate(const bringup_field_t *f, const char *value, char *err, size_t errlen);
// Raw read (no default) / write without validation.
bool bu_read(const char *key, char *out, size_t len);
esp_err_t bu_write(const char *key, const char *value);

esp_err_t bu_wifi_start(bool bringup);
bool bu_wifi_sta_connected(void);
const char *bu_wifi_ip(void);
const char *bu_wifi_ap_ssid(void);
// Scan; fills a JSON array into buf. Returns false on failure.
bool bu_wifi_scan_json(char *buf, size_t len);
esp_err_t bu_dns_start(void);
void bu_dns_stop(void);
