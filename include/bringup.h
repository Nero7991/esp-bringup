#pragma once

// esp-bringup: first-boot and recovery setup for ESP-IDF devices.
//
// A project describes its settings as a table of fields. The component stores them in the
// project's NVS namespace, serves a setup page generated from the table, and owns Wi-Fi: station
// mode with the saved credentials, and a WPA2 hotspot with a captive portal while the device is in
// bring-up mode or cannot reach its network.
//
// Typical use:
//     nvs_flash_init(); esp_netif_init(); esp_event_loop_create_default();
//     bringup_init(&cfg);                     // decides the mode and starts Wi-Fi
//     ... start the project's httpd (max_uri_handlers >= own + BRINGUP_HTTP_HANDLERS) ...
//     bringup_register_http(server);
//     if (bringup_active()) { ... run only what makes sense without settings ... }

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// URI handlers bringup_register_http() adds to the project's server.
#define BRINGUP_HTTP_HANDLERS 7

// Built-in setting keys, always present, listed before the project's fields.
#define BRINGUP_KEY_WIFI_SSID  "wifi_ssid"
#define BRINGUP_KEY_WIFI_PASS  "wifi_pass"
#define BRINGUP_KEY_NAME       "name"
#define BRINGUP_KEY_ADMIN_PASS "admin_pass"

// User name for HTTP Basic auth; the password is the admin_pass setting.
#define BRINGUP_ADMIN_USER "admin"

typedef enum {
    BRINGUP_T_STRING, // free text, max length = max (0: 63)
    BRINGUP_T_MAC,    // aa:bb:cc:dd:ee:ff
    BRINGUP_T_HEX,    // exactly max bytes as 2*max hex digits (e.g. a 16-byte key)
    BRINGUP_T_INT,    // integer in [min, max]
    BRINGUP_T_BOOL,   // "0" or "1"
    BRINGUP_T_CHOICE, // one of the '|' separated values in choices
} bringup_type_t;

// ---- detect: a project-specific "find this value" flow for a field ----
//
// The page shows a button next to the field. Pressing it opens a dialog with the prompt, calls
// start() with the values currently in the form (saved or not), then polls about once a second and
// lists what poll() reports; the user picks one and it fills the field. Closing the dialog or the
// timeout calls stop(). The callbacks run on the HTTP server task: keep them short.

#define BRINGUP_DETECT_MAX 6

typedef enum {
    BRINGUP_DETECT_WAITING, // still looking; found[] may already hold candidates
    BRINGUP_DETECT_DONE,    // finished; found[] holds the result
    BRINGUP_DETECT_FAILED,  // message says why
} bringup_detect_state_t;

typedef struct {
    bringup_detect_state_t state;
    char message[96]; // shown under the prompt (optional)
    uint8_t n;
    struct {
        char value[64]; // what goes into the field
        char label[64]; // what the user sees, e.g. "Game controller, 3 requests"
    } found[BRINGUP_DETECT_MAX];
} bringup_detect_result_t;

// The form's current values, passed to start(). Falls back to the stored value for keys the form
// did not send.
typedef struct bringup_values bringup_values_t;
bool bringup_values_get(const bringup_values_t *v, const char *key, char *out, size_t len);

typedef struct {
    const char *button;  // button text, e.g. "Detect"
    const char *title;   // dialog title
    const char *prompt;  // instruction, e.g. "Press the PS button on the controller."
    uint16_t timeout_s;  // stop polling after this (0: 60)
    // Return ESP_OK to start, or an error with a message for the user in err.
    esp_err_t (*start)(const char *key, const bringup_values_t *form, char *err, size_t errlen);
    void (*poll)(const char *key, bringup_detect_result_t *out);
    void (*stop)(const char *key);
} bringup_detect_t;

typedef struct {
    const char *key;      // NVS key, at most 15 characters, unique
    const char *label;    // shown on the page
    const char *help;     // optional hint under the field
    const char *group;    // section heading; consecutive fields with the same group share it
    bringup_type_t type;
    const char *def;      // default as text, or NULL
    bool required;        // a missing value puts the device in bring-up mode
    bool secret;          // never sent back to the page or logged; empty input keeps the old value
    int32_t min, max;     // see bringup_type_t
    const char *choices;  // BRINGUP_T_CHOICE: "a|b|c"
    const bringup_detect_t *detect; // optional "find this value" flow, see above
} bringup_field_t;

typedef struct {
    const char *product;         // page title, e.g. "WoL via BT"
    const char *ap_ssid_prefix;  // hotspot SSID is "<prefix>-XXXX" (last two MAC bytes)
    const char *ap_password;     // WPA2 hotspot password, 8 to 63 characters
    const char *hostname_prefix; // DHCP hostname "<prefix>-xxxx"; NULL keeps the IDF default
    const char *nvs_namespace;   // where the settings live, at most 15 characters
    const bringup_field_t *fields;
    size_t n_fields;
    uint8_t sta_fail_limit;      // failed connects before the fallback hotspot (0: 5)
    uint8_t power_cycles;        // boots ended within the window in a row; the next boot
                                 // enters bring-up (0: 3; 255: off)
    uint16_t power_cycle_window_s; // uptime after which a boot no longer counts (0: 10)
    // Optional: called inside bringup_init() before the mode decision. Use bringup_seed() here to
    // fill settings from build-time defaults or migrate them from an older firmware's storage.
    void (*on_seed)(void);
    // Optional: an outside reason to enter bring-up this boot (e.g. esp-recover's power-cycle
    // trigger). Return a short reason, or NULL/"" for none. Set power_cycles = 255 when another
    // component counts power cycles.
    const char *(*external_trigger)(void);
} bringup_config_t;

// Call once after nvs_flash_init, esp_netif_init and esp_event_loop_create_default. Decides
// whether the device is in bring-up mode and starts Wi-Fi accordingly. The config and field table
// must stay valid for the life of the program.
esp_err_t bringup_init(const bringup_config_t *cfg);

// Register /setup and /api/setup* on the project's server, and the captive-portal redirect for
// unknown URLs while the hotspot is up.
esp_err_t bringup_register_http(httpd_handle_t server);

bool bringup_active(void);      // in bring-up mode this boot
const char *bringup_reason(void); // why ("not configured: bt_pad", "requested", "power cycles",
                                  // the external trigger's reason, or "")
bool bringup_hotspot_up(void);  // hotspot running (bring-up mode or Wi-Fi fallback)
bool bringup_wifi_connected(void);

// Settings. Getters fall back to the field's default; they return false (or the def argument)
// when there is no value. Setters validate against the field and commit to NVS.
bool bringup_get_str(const char *key, char *out, size_t len);
bool bringup_get_mac(const char *key, uint8_t mac[6]);
bool bringup_get_hex(const char *key, uint8_t *out, size_t n);
int32_t bringup_get_int(const char *key, int32_t def);
bool bringup_get_bool(const char *key, bool def);
bool bringup_is_set(const char *key);
esp_err_t bringup_set_str(const char *key, const char *value); // ESP_ERR_INVALID_ARG if invalid
esp_err_t bringup_set_mac(const char *key, const uint8_t mac[6]);
esp_err_t bringup_set_hex(const char *key, const uint8_t *data, size_t n);
// Store value only if the key has none (first-boot seeding, migration from older firmware).
esp_err_t bringup_seed(const char *key, const char *value);

// Reboot into bring-up mode.
void bringup_enter(void);

// HTTP Basic auth against admin:<admin_pass>. True when no admin password is set yet (a fresh
// device, reachable only through its WPA2 hotspot). Sends the 401 itself when it returns false.
bool bringup_auth(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
