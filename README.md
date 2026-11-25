# esp-bringup

First-boot and recovery setup for ESP-IDF devices: Wi-Fi, a WPA2 setup hotspot with a captive
portal, and a settings page generated from a table the project defines.

Each project lists its own settings (NVS keys, labels, types, required or optional, secret or not).
The component stores them in the project's NVS namespace, validates them, and serves one page that
works on a phone. Wi-Fi network, Wi-Fi password, device name and admin password are built in.

Status: in use on [esp32-WoLviaBT](https://github.com/Nero7991/esp32-WoLviaBT) (ESP32, ESP-IDF
v5.5.6). Modelled on the QuickVolt bring-up flow.

## What it does

- **Bring-up mode** starts the hotspot `<prefix>-XXXX` (last two bytes of the AP MAC) at
  `192.168.4.1`. A DNS server answers every name with that address and unknown URLs redirect to
  `/setup`, so phones open the page by themselves. If Wi-Fi credentials are saved, the device keeps
  joining that network too, so it stays reachable there.
- **Entered when:** a required setting is empty (a fresh board), `POST /api/setup/action?do=bringup`,
  or N quick power cycles in a row (default 3: three boots that each end within 10 s, then the
  fourth boot enters bring-up).
- **Wi-Fi fallback:** after N failed connects (default 5) the hotspot comes up next to the
  retries, and goes away again once the network is back.
- **Page** `/setup`: Apple-style, light and dark, Wi-Fi scan, per-field validation messages, secrets
  never sent back (an empty secret field keeps the stored value), save and restart.
- **Auth:** HTTP Basic, user `admin`, password = the `admin_pass` setting. Until one is set (a
  fresh board) the API is open, protected only by the hotspot's WPA2 password. The project can use
  `bringup_auth()` for its own endpoints.

## Use

```c
#include "bringup.h"

static const bringup_field_t fields[] = {
    { .key = "target", .label = "Target MAC", .group = "Wake",
      .type = BRINGUP_T_MAC, .required = true },
    { .key = "token", .label = "API token", .group = "Cloud",
      .type = BRINGUP_T_STRING, .secret = true, .max = 40 },
    { .key = "period", .label = "Report period (s)", .type = BRINGUP_T_INT,
      .min = 1, .max = 3600, .def = "60" },
};

static const bringup_config_t setup = {
    .product = "My device",
    .ap_ssid_prefix = "MyDevice",
    .ap_password = "mydevice",          // WPA2, at least 8 characters
    .hostname_prefix = "mydevice",
    .nvs_namespace = "mydev",
    .fields = fields,
    .n_fields = sizeof(fields) / sizeof(fields[0]),
};

void app_main(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    bringup_init(&setup);                 // decides the mode, starts Wi-Fi

    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.max_uri_handlers = 4 + BRINGUP_HTTP_HANDLERS;
    httpd_handle_t server;
    httpd_start(&server, &c);
    bringup_register_http(server);       // /setup, /api/setup*, captive redirect

    uint8_t mac[6];
    if (!bringup_active() && bringup_get_mac("target", mac)) {
        /* normal operation */
    }
}
```

Field types: `BRINGUP_T_STRING` (length `min`..`max`), `BRINGUP_T_MAC`, `BRINGUP_T_HEX` (`max`
bytes), `BRINGUP_T_INT` (`min`..`max`), `BRINGUP_T_BOOL`, `BRINGUP_T_CHOICE` (`choices = "a|b|c"`).
Keys are NVS keys (15 characters at most); keys starting with `_` are reserved.

A field can have a `detect` flow: a button next to it opens a dialog with the project's prompt, the
component calls the project's `start()` with the form's current values, polls `poll()` about once a
second and lists what it reports, and the user picks one to fill the field. Closing the dialog or
the timeout calls `stop()`. esp32-WoLviaBT uses it to find a controller's address: "press the PS
button" and the board lists whatever pages it.

`on_seed` in the config runs before the mode decision: call `bringup_seed(key, value)` there to fill
settings from build-time defaults or to migrate an older firmware's storage. It only writes keys
that have no value.

## HTTP API

| Method | Path | |
|---|---|---|
| GET | `/setup` | the page |
| GET | `/api/setup` | product, mode, reason, Wi-Fi state, every field (`value` empty for secrets, `set` tells whether one is stored) |
| POST | `/api/setup` | `{"values":{"key":"value",...},"reboot":true}`. All values are validated first; nothing is written unless all are valid. Errors come back as `{"ok":false,"errors":{"key":"message"}}` |
| GET | `/api/setup/scan` | `[{"ssid","rssi","secure"}]` |
| POST | `/api/setup/action?do=reboot` or `?do=bringup` | restart, or restart into bring-up mode |
| POST | `/api/setup/detect?key=K&do=start` (body `{"values":{...}}`) or `&do=stop` | start or stop a field's detect flow |
| GET | `/api/setup/detect?key=K` | `{"state":"waiting"/"done"/"failed","message","found":[{"value","label"}]}` |

## Requirements

ESP-IDF 5.x with `esp_wifi`, `esp_http_server`, `json` (cJSON) and `mbedtls`. The component owns
Wi-Fi: the project must not call `esp_wifi_init` itself. It needs `BRINGUP_HTTP_HANDLERS` free URI
handler slots on the project's server and installs the server's 404 handler.

Set `lru_purge_enable = true` on the server and consider `CONFIG_LWIP_MAX_SOCKETS=16`. Phones on
the hotspot open several captive-portal probe connections and leave them idle; with the defaults
(10 sockets, no purge) they filled the socket table on an ESP32 and every new connection failed
with `httpd_accept_conn: error in accept (23)`.

## License

MIT, see `LICENSE`.
