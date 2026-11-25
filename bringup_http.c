// bringup_http.c - the setup page and its API, registered on the project's HTTP server.
//
//   GET  /setup                 the page (generated in the browser from GET /api/setup)
//   GET  /api/setup             product, mode, Wi-Fi state and every field (secrets never sent)
//   POST /api/setup             {"values":{key:value,...},"reboot":true|false}
//   GET  /api/setup/scan        nearby Wi-Fi networks
//   POST /api/setup/action      ?do=reboot | ?do=bringup
//   POST /api/setup/detect      ?key=K&do=start (body {"values":{...}}) | ?key=K&do=stop
//   GET  /api/setup/detect      ?key=K   poll
// While the hotspot is up, unknown URLs redirect to /setup (captive portal).

#include "bringup_priv.h"

#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "bringup_http";

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[] asm("_binary_setup_html_end");

static const char *type_name(bringup_type_t t)
{
    static const char *n[] = { "string", "mac", "hex", "int", "bool", "choice" };
    return (unsigned)t < sizeof(n) / sizeof(n[0]) ? n[t] : "string";
}

// ---- auth ----

static bool ct_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char d = la != lb;
    for (size_t i = 0; i < la && i < lb; i++) {
        d |= (unsigned char)(a[i] ^ b[i]);
    }
    return d == 0;
}

bool bringup_auth(httpd_req_t *req)
{
    char pass[BRINGUP_VAL_MAX];
    if (!bu_read(BRINGUP_KEY_ADMIN_PASS, pass, sizeof(pass))) {
        return true; // fresh device: only reachable through the WPA2 hotspot or a trusted LAN
    }
    bool ok = false;
    char hdr[200];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Basic ", 6) == 0) {
        unsigned char dec[160];
        size_t dl = 0;
        if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &dl, (const unsigned char *)hdr + 6,
                                  strlen(hdr + 6)) == 0) {
            dec[dl] = '\0';
            char want[BRINGUP_VAL_MAX + 8];
            snprintf(want, sizeof(want), "%s:%s", BRINGUP_ADMIN_USER, pass);
            ok = ct_equal((const char *)dec, want);
            memset(want, 0, sizeof(want));
        }
        memset(dec, 0, sizeof(dec));
    }
    memset(pass, 0, sizeof(pass));
    if (!ok) {
        httpd_resp_set_status(req, "401 Unauthorized");
        // The setup page sends X-Bringup and shows its own sign-in: captive-portal browsers do not
        // show the native prompt, and a full browser would otherwise show both.
        if (httpd_req_get_hdr_value_len(req, "X-Bringup") == 0) {
            httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"setup\"");
        }
        httpd_resp_sendstr(req, "Unauthorized\n");
    }
    return ok;
}

// ---- handlers ----

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, setup_html_start, setup_html_end - setup_html_start);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, s);
    cJSON_free(s);
    return e;
}

static esp_err_t setup_get(httpd_req_t *req)
{
    if (!bringup_auth(req)) {
        return ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "product", g_bu_cfg->product ? g_bu_cfg->product : "Device");
    cJSON_AddBoolToObject(root, "bringup", bringup_active());
    cJSON_AddStringToObject(root, "reason", bringup_reason());
    cJSON_AddBoolToObject(root, "hotspot", bringup_hotspot_up());
    cJSON_AddStringToObject(root, "ap_ssid", bu_wifi_ap_ssid());
    cJSON_AddBoolToObject(root, "wifi_connected", bu_wifi_sta_connected());
    cJSON_AddStringToObject(root, "ip", bu_wifi_ip());
    cJSON_AddBoolToObject(root, "auth", bringup_is_set(BRINGUP_KEY_ADMIN_PASS));
    cJSON *arr = cJSON_AddArrayToObject(root, "fields");
    for (size_t i = 0; i < g_bu_nfields; i++) {
        const bringup_field_t *f = g_bu_fields[i];
        cJSON *o = cJSON_CreateObject();
        char v[BRINGUP_VAL_MAX];
        bool set = bu_read(f->key, v, sizeof(v));
        cJSON_AddStringToObject(o, "key", f->key);
        cJSON_AddStringToObject(o, "label", f->label ? f->label : f->key);
        if (f->help) cJSON_AddStringToObject(o, "help", f->help);
        cJSON_AddStringToObject(o, "group", f->group ? f->group : "");
        cJSON_AddStringToObject(o, "type", type_name(f->type));
        cJSON_AddBoolToObject(o, "required", f->required);
        cJSON_AddBoolToObject(o, "secret", f->secret);
        cJSON_AddNumberToObject(o, "min", f->min);
        cJSON_AddNumberToObject(o, "max", f->max);
        if (f->choices) cJSON_AddStringToObject(o, "choices", f->choices);
        if (f->detect) {
            cJSON *d = cJSON_AddObjectToObject(o, "detect");
            cJSON_AddStringToObject(d, "button", f->detect->button ? f->detect->button : "Detect");
            cJSON_AddStringToObject(d, "title", f->detect->title ? f->detect->title : f->label);
            cJSON_AddStringToObject(d, "prompt", f->detect->prompt ? f->detect->prompt : "");
            cJSON_AddNumberToObject(d, "timeout", f->detect->timeout_s ? f->detect->timeout_s : 60);
        }
        if (f->def) cJSON_AddStringToObject(o, "default", f->def);
        cJSON_AddBoolToObject(o, "set", set);
        cJSON_AddStringToObject(o, "value", (set && !f->secret) ? v : "");
        memset(v, 0, sizeof(v));
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, root);
}

// 4096: bringup_enter() logs and writes NVS on this stack, and a project's log hook (netserial)
// runs on it too. At 2048 it overflowed and the device rebooted by panic without the flag set.
static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(800)); // let the response leave
    if (arg) {
        bringup_enter();
    }
    esp_restart();
}

static esp_err_t setup_post(httpd_req_t *req)
{
    if (!bringup_auth(req)) {
        return ESP_OK;
    }
    if (req->content_len == 0 || req->content_len > 4096) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    char *body = calloc(1, req->content_len + 1);
    if (!body) {
        return httpd_resp_send_500(req);
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += r;
    }
    cJSON *in = cJSON_Parse(body);
    memset(body, 0, got);
    free(body);
    cJSON *values = in ? cJSON_GetObjectItem(in, "values") : NULL;
    if (!cJSON_IsObject(values)) {
        cJSON_Delete(in);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected {\"values\":{...}}");
    }

    // Validate everything first; write nothing unless all values are valid.
    cJSON *errs = cJSON_CreateObject();
    int nerr = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, values) {
        const bringup_field_t *f = bu_field(it->string);
        char msg[64];
        if (!f) {
            cJSON_AddStringToObject(errs, it->string, "unknown setting");
            nerr++;
        } else if (!cJSON_IsString(it)) {
            cJSON_AddStringToObject(errs, it->string, "must be a string");
            nerr++;
        } else if (f->secret && it->valuestring[0] == '\0') {
            continue; // empty secret: keep the stored one
        } else if (!bu_validate(f, it->valuestring, msg, sizeof(msg))) {
            cJSON_AddStringToObject(errs, it->string, msg);
            nerr++;
        }
    }
    cJSON *out = cJSON_CreateObject();
    if (nerr) {
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddItemToObject(out, "errors", errs);
        cJSON_Delete(in);
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, out);
    }
    cJSON_Delete(errs);
    int saved = 0;
    cJSON_ArrayForEach(it, values) {
        const bringup_field_t *f = bu_field(it->string);
        if (f->secret && it->valuestring[0] == '\0') {
            continue;
        }
        if ((it->valuestring[0] ? bringup_set_str(f->key, it->valuestring)
                                : bu_write(f->key, "")) == ESP_OK) {
            saved++;
        }
        if (f->secret) {
            memset(it->valuestring, 0, strlen(it->valuestring));
        }
    }
    bool reboot = cJSON_IsTrue(cJSON_GetObjectItem(in, "reboot"));
    cJSON_Delete(in);
    ESP_LOGI(TAG, "saved %d setting(s)%s", saved, reboot ? ", restarting" : "");
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddNumberToObject(out, "saved", saved);
    cJSON_AddBoolToObject(out, "reboot", reboot);
    esp_err_t e = send_json(req, out);
    if (reboot) {
        xTaskCreate(restart_task, "bu_restart", 4096, NULL, 5, NULL);
    }
    return e;
}

static esp_err_t scan_get(httpd_req_t *req)
{
    if (!bringup_auth(req)) {
        return ESP_OK;
    }
    char *buf = malloc(2560);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    if (!bu_wifi_scan_json(buf, 2560)) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, buf);
    free(buf);
    return e;
}

static esp_err_t action_post(httpd_req_t *req)
{
    if (!bringup_auth(req)) {
        return ESP_OK;
    }
    char q[32], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "do", v, sizeof(v)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "do=reboot|bringup");
    }
    bool to_bringup = strcmp(v, "bringup") == 0;
    if (!to_bringup && strcmp(v, "reboot") != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "do=reboot|bringup");
    }
    httpd_resp_sendstr(req, to_bringup ? "restarting into bring-up mode\n" : "restarting\n");
    xTaskCreate(restart_task, "bu_restart", 4096, to_bringup ? (void *)1 : NULL, 5, NULL);
    return ESP_OK;
}

// ---- detect ----

struct bringup_values {
    const cJSON *obj;
};

bool bringup_values_get(const bringup_values_t *v, const char *key, char *out, size_t len)
{
    const cJSON *it = v && v->obj ? cJSON_GetObjectItemCaseSensitive(v->obj, key) : NULL;
    if (cJSON_IsString(it) && it->valuestring[0]) {
        snprintf(out, len, "%s", it->valuestring);
        return true;
    }
    return bringup_get_str(key, out, len);
}

static const bringup_field_t *detect_field(httpd_req_t *req, char *q, size_t qlen, char *op,
                                           size_t oplen)
{
    char key[16];
    op[0] = '\0';
    if (httpd_req_get_url_query_str(req, q, qlen) != ESP_OK ||
        httpd_query_key_value(q, "key", key, sizeof(key)) != ESP_OK) {
        return NULL;
    }
    httpd_query_key_value(q, "do", op, oplen);
    const bringup_field_t *f = bu_field(key);
    return f && f->detect ? f : NULL;
}

static esp_err_t detect_post(httpd_req_t *req)
{
    if (!bringup_auth(req)) {
        return ESP_OK;
    }
    char q[64], op[8];
    const bringup_field_t *f = detect_field(req, q, sizeof(q), op, sizeof(op));
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "key=<field with detect>");
    }
    if (strcmp(op, "stop") == 0) {
        if (f->detect->stop) {
            f->detect->stop(f->key);
        }
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    if (strcmp(op, "start") != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "do=start|stop");
    }
    cJSON *in = NULL;
    if (req->content_len > 0 && req->content_len <= 4096) {
        char *body = calloc(1, req->content_len + 1);
        size_t got = 0;
        while (body && got < req->content_len) {
            int r = httpd_req_recv(req, body + got, req->content_len - got);
            if (r <= 0) {
                break;
            }
            got += r;
        }
        if (body) {
            in = cJSON_Parse(body);
            memset(body, 0, got);
            free(body);
        }
    }
    bringup_values_t vals = { .obj = in ? cJSON_GetObjectItem(in, "values") : NULL };
    char err[96] = "";
    esp_err_t e = f->detect->start ? f->detect->start(f->key, &vals, err, sizeof(err)) : ESP_FAIL;
    cJSON_Delete(in);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", e == ESP_OK);
    if (e != ESP_OK) {
        cJSON_AddStringToObject(out, "error", err[0] ? err : esp_err_to_name(e));
    }
    return send_json(req, out);
}

static esp_err_t detect_get(httpd_req_t *req)
{
    if (!bringup_auth(req)) {
        return ESP_OK;
    }
    char q[64], op[8];
    const bringup_field_t *f = detect_field(req, q, sizeof(q), op, sizeof(op));
    if (!f || !f->detect->poll) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "key=<field with detect>");
    }
    bringup_detect_result_t *r = calloc(1, sizeof(*r));
    if (!r) {
        return httpd_resp_send_500(req);
    }
    f->detect->poll(f->key, r);
    static const char *states[] = { "waiting", "done", "failed" };
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "state", (unsigned)r->state < 3 ? states[r->state] : "failed");
    cJSON_AddStringToObject(out, "message", r->message);
    cJSON *arr = cJSON_AddArrayToObject(out, "found");
    for (int i = 0; i < r->n && i < BRINGUP_DETECT_MAX; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "value", r->found[i].value);
        cJSON_AddStringToObject(o, "label", r->found[i].label);
        cJSON_AddItemToArray(arr, o);
    }
    free(r);
    return send_json(req, out);
}

static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    if (bringup_hotspot_up()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
        return httpd_resp_send(req, NULL, 0);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

esp_err_t bringup_register_http(httpd_handle_t server)
{
    static const httpd_uri_t uris[BRINGUP_HTTP_HANDLERS] = {
        { .uri = "/setup", .method = HTTP_GET, .handler = page_get },
        { .uri = "/api/setup", .method = HTTP_GET, .handler = setup_get },
        { .uri = "/api/setup", .method = HTTP_POST, .handler = setup_post },
        { .uri = "/api/setup/scan", .method = HTTP_GET, .handler = scan_get },
        { .uri = "/api/setup/action", .method = HTTP_POST, .handler = action_post },
        { .uri = "/api/setup/detect", .method = HTTP_POST, .handler = detect_post },
        { .uri = "/api/setup/detect", .method = HTTP_GET, .handler = detect_get },
    };
    for (size_t i = 0; i < BRINGUP_HTTP_HANDLERS; i++) {
        esp_err_t e = httpd_register_uri_handler(server, &uris[i]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "register %s: %s (raise max_uri_handlers?)", uris[i].uri,
                     esp_err_to_name(e));
            return e;
        }
    }
    return httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
}
