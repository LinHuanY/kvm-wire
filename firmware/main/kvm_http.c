/*
 * 配置网页：AP 模式下访问 http://192.168.4.1/
 * 提供状态、WiFi 扫描/配置、PC 目标配置、BLE 扫描/连接。
 */
#include "kvm_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kvm_ble.h"
#include "kvm_config.h"
#include "kvm_input.h"
#include "kvm_net.h"
#include "kvm_proto.h"
#include "kvm_wan.h"
#include "kvm_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "kvm_http";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* ------------------------------------------------------------------ */

static cJSON *body_json(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 2048) {
        return NULL;
    }
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        return NULL;
    }
    int received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, buf + received, len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            return NULL;
        }
        received += r;
    }
    buf[received] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

static esp_err_t reply_json(httpd_req_t *req, cJSON *root)
{
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t err = ESP_FAIL;
    if (out) {
        err = httpd_resp_sendstr(req, out);
        cJSON_free(out);
    }
    cJSON_Delete(root);
    return err;
}

static esp_err_t reply_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    char *out = cJSON_PrintUnformatted(o);
    esp_err_t err = httpd_resp_sendstr(req, out ? out : "{}");
    if (out) {
        cJSON_free(out);
    }
    cJSON_Delete(o);
    return err;
}

static bool parse_addr(const char *text, uint8_t out[6])
{
    unsigned v[6];
    if (text == NULL || sscanf(text, "%02x:%02x:%02x:%02x:%02x:%02x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5])
            != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF) {
            return false;
        }
        out[i] = (uint8_t)v[i];
    }
    return true;
}

static void addr_to_str(const uint8_t bda[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

/* ------------------------------------------------------------------ */
/* 页面                                                                */
/* ------------------------------------------------------------------ */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
}

static esp_err_t h_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* 状态                                                                */
/* ------------------------------------------------------------------ */

static esp_err_t h_status(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "fw", "1.0.0");
    cJSON_AddNumberToObject(o, "uptime_s", esp_timer_get_time() / 1000000);

    cJSON *ap = cJSON_AddObjectToObject(o, "ap");
    cJSON_AddStringToObject(ap, "ssid", kvm_config_ap_ssid());
    cJSON_AddStringToObject(ap, "ip", "192.168.4.1");

    cJSON *wifi = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddBoolToObject(wifi, "configured", g_cfg.sta_ssid[0] != '\0');
    cJSON_AddBoolToObject(wifi, "up", kvm_wifi_sta_up());
    cJSON_AddStringToObject(wifi, "ssid", kvm_wifi_sta_ssid());
    cJSON_AddStringToObject(wifi, "ip", kvm_wifi_sta_ip());
    cJSON_AddNumberToObject(wifi, "rssi", kvm_wifi_sta_rssi());

    cJSON *host = cJSON_AddObjectToObject(o, "host");
    cJSON_AddStringToObject(host, "ip", g_cfg.host_ip);
    cJSON_AddNumberToObject(host, "port", g_cfg.host_port);
    char token[17];
    snprintf(token, sizeof(token), "%016llx", (unsigned long long)g_cfg.token);
    cJSON_AddStringToObject(host, "token", token);
    cJSON_AddBoolToObject(host, "online", kvm_net_host_online());
    cJSON_AddNumberToObject(host, "idle_ms", kvm_net_host_idle_ms());
    cJSON_AddNumberToObject(host, "tx", kvm_net_tx_count());
    cJSON_AddNumberToObject(host, "rx", kvm_net_rx_count());

    cJSON *wan = cJSON_AddObjectToObject(o, "wan");
    cJSON_AddBoolToObject(wan, "enabled", g_cfg.wan_enabled);
    cJSON_AddBoolToObject(wan, "connected", kvm_wan_connected());
    cJSON_AddStringToObject(wan, "host", g_cfg.relay_host);
    cJSON_AddNumberToObject(wan, "port", g_cfg.relay_port);
    cJSON_AddStringToObject(wan, "status", kvm_wan_status_text());
    cJSON_AddBoolToObject(wan, "key_set", g_cfg.relay_key[0] != '\0');

    cJSON *ble = cJSON_AddObjectToObject(o, "ble");
    cJSON_AddBoolToObject(ble, "scanning", kvm_ble_scanning());
    cJSON *arr = cJSON_AddArrayToObject(ble, "connected");
    kvm_ble_conn_t conns[KVM_BLE_MAX_CONNS];
    int n = kvm_ble_conn_get(conns, KVM_BLE_MAX_CONNS);
    for (int i = 0; i < n; i++) {
        if (!conns[i].connected) {
            continue;
        }
        cJSON *d = cJSON_CreateObject();
        char addr[18];
        addr_to_str(conns[i].bda, addr);
        cJSON_AddStringToObject(d, "name", conns[i].name);
        cJSON_AddStringToObject(d, "addr", addr);
        cJSON_AddNumberToObject(d, "usage", conns[i].usage);
        cJSON_AddNumberToObject(d, "battery", conns[i].battery);
        cJSON_AddNumberToObject(d, "rssi", conns[i].rssi);
        cJSON_AddItemToArray(arr, d);
    }
    return reply_json(req, o);
}

/* ------------------------------------------------------------------ */
/* WiFi                                                                */
/* ------------------------------------------------------------------ */

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "scanning", kvm_wifi_scanning());
    cJSON *arr = cJSON_AddArrayToObject(o, "results");
    kvm_wifi_ap_t aps[KVM_WIFI_MAX_AP];
    int n = kvm_wifi_scan_get(aps, KVM_WIFI_MAX_AP);
    for (int i = 0; i < n; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(a, "rssi", aps[i].rssi);
        cJSON_AddNumberToObject(a, "auth", aps[i].authmode);
        cJSON_AddItemToArray(arr, a);
    }
    return reply_json(req, o);
}

static esp_err_t h_wifi_scan_start(httpd_req_t *req)
{
    esp_err_t err = kvm_wifi_scan_start();
    if (err != ESP_OK) {
        return reply_error(req, "409 Conflict", "扫描已在进行");
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return reply_json(req, o);
}

static void wifi_apply_task(void *arg)
{
    (void)arg;
    /* 等 HTTP 响应先发出去，再切信道/重连，避免网页看到 Failed to fetch */
    vTaskDelay(pdMS_TO_TICKS(600));
    kvm_wifi_apply_sta();
    vTaskDelete(NULL);
}

static esp_err_t h_wifi_set(httpd_req_t *req)
{
    cJSON *body = body_json(req);
    if (!body) {
        return reply_error(req, "400 Bad Request", "JSON 解析失败");
    }
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(body, "ssid");
    cJSON *pass = cJSON_GetObjectItemCaseSensitive(body, "password");
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        cJSON_Delete(body);
        return reply_error(req, "400 Bad Request", "缺少 ssid");
    }
    esp_err_t err = kvm_config_set_wifi(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "");
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return reply_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "applying", true);
    reply_json(req, o);
    xTaskCreate(wifi_apply_task, "wifi_apply", 3072, NULL, 5, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* PC 目标                                                             */
/* ------------------------------------------------------------------ */

static esp_err_t h_host_set(httpd_req_t *req)
{
    cJSON *body = body_json(req);
    if (!body) {
        return reply_error(req, "400 Bad Request", "JSON 解析失败");
    }
    cJSON *ip = cJSON_GetObjectItemCaseSensitive(body, "ip");
    cJSON *port = cJSON_GetObjectItemCaseSensitive(body, "port");
    cJSON *token = cJSON_GetObjectItemCaseSensitive(body, "token");

    const char *ip_str = cJSON_IsString(ip) ? ip->valuestring : "";
    if (ip_str[0] != '\0') {
        struct in_addr addr;
        if (inet_pton(AF_INET, ip_str, &addr) != 1) {
            cJSON_Delete(body);
            return reply_error(req, "400 Bad Request", "IP 格式不正确");
        }
    }
    uint16_t port_val = g_cfg.host_port;
    if (cJSON_IsNumber(port)) {
        int p = port->valueint;
        if (p <= 0 || p > 65535) {
            cJSON_Delete(body);
            return reply_error(req, "400 Bad Request", "端口不正确");
        }
        port_val = (uint16_t)p;
    }
    uint64_t token_val = g_cfg.token;
    if (cJSON_IsString(token) && token->valuestring[0] != '\0') {
        char *end = NULL;
        token_val = strtoull(token->valuestring, &end, 16);
        if (end == token->valuestring || *end != '\0' || strlen(token->valuestring) != 16) {
            cJSON_Delete(body);
            return reply_error(req, "400 Bad Request", "token 必须是 16 位十六进制");
        }
    }
    kvm_config_set_host(ip_str, port_val, token_val, true);
    kvm_net_apply_config();
    cJSON_Delete(body);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return reply_json(req, o);
}

/* ------------------------------------------------------------------ */
/* BLE                                                                 */
/* ------------------------------------------------------------------ */

static esp_err_t h_ble_scan_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "scanning", kvm_ble_scanning());
    cJSON *arr = cJSON_AddArrayToObject(o, "results");
    kvm_ble_scan_dev_t devs[KVM_BLE_MAX_SCAN];
    int n = kvm_ble_scan_get(devs, KVM_BLE_MAX_SCAN);
    for (int i = 0; i < n; i++) {
        cJSON *d = cJSON_CreateObject();
        char addr[18];
        addr_to_str(devs[i].bda, addr);
        cJSON_AddStringToObject(d, "name", devs[i].name);
        cJSON_AddStringToObject(d, "addr", addr);
        cJSON_AddNumberToObject(d, "rssi", devs[i].rssi);
        cJSON_AddNumberToObject(d, "type", devs[i].addr_type);
        cJSON_AddNumberToObject(d, "usage", devs[i].usage);
        cJSON_AddNumberToObject(d, "appearance", devs[i].appearance);
        cJSON_AddBoolToObject(d, "hid", devs[i].hid);
        cJSON_AddItemToArray(arr, d);
    }
    return reply_json(req, o);
}

static esp_err_t h_ble_scan_start(httpd_req_t *req)
{
    uint32_t duration = 8000;
    cJSON *body = body_json(req);
    if (body) {
        cJSON *d = cJSON_GetObjectItemCaseSensitive(body, "duration_ms");
        if (cJSON_IsNumber(d)) {
            duration = (uint32_t)d->valueint;
        }
        cJSON_Delete(body);
    }
    if (duration < 2000) {
        duration = 2000;
    }
    if (duration > 30000) {
        duration = 30000;
    }
    esp_err_t err = kvm_ble_scan_start(duration);
    if (err != ESP_OK) {
        return reply_error(req, "409 Conflict", "扫描已在进行");
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return reply_json(req, o);
}

static esp_err_t h_ble_connect(httpd_req_t *req)
{
    cJSON *body = body_json(req);
    if (!body) {
        return reply_error(req, "400 Bad Request", "JSON 解析失败");
    }
    cJSON *addr = cJSON_GetObjectItemCaseSensitive(body, "addr");
    cJSON *type = cJSON_GetObjectItemCaseSensitive(body, "type");
    uint8_t bda[6];
    if (!cJSON_IsString(addr) || !parse_addr(addr->valuestring, bda)) {
        cJSON_Delete(body);
        return reply_error(req, "400 Bad Request", "地址格式不正确");
    }
    uint8_t addr_type = cJSON_IsNumber(type) ? (uint8_t)type->valueint : 0;
    esp_err_t err = kvm_ble_connect(bda, addr_type);
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return reply_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return reply_json(req, o);
}

static esp_err_t h_ble_disconnect(httpd_req_t *req)
{
    cJSON *body = body_json(req);
    if (!body) {
        return reply_error(req, "400 Bad Request", "JSON 解析失败");
    }
    cJSON *addr = cJSON_GetObjectItemCaseSensitive(body, "addr");
    uint8_t bda[6];
    if (!cJSON_IsString(addr) || !parse_addr(addr->valuestring, bda)) {
        cJSON_Delete(body);
        return reply_error(req, "400 Bad Request", "地址格式不正确");
    }
    kvm_ble_disconnect(bda);
    cJSON_Delete(body);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return reply_json(req, o);
}

/* ------------------------------------------------------------------ */
/* 广域网中继                                                          */
/* ------------------------------------------------------------------ */

static esp_err_t h_wan_set(httpd_req_t *req)
{
    cJSON *body = body_json(req);
    if (!body) {
        return reply_error(req, "400 Bad Request", "JSON 解析失败");
    }
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(body, "enabled");
    cJSON *host = cJSON_GetObjectItemCaseSensitive(body, "host");
    cJSON *port = cJSON_GetObjectItemCaseSensitive(body, "port");
    cJSON *key = cJSON_GetObjectItemCaseSensitive(body, "key");

    const char *host_str = cJSON_IsString(host) ? host->valuestring : g_cfg.relay_host;
    int port_val = cJSON_IsNumber(port) ? port->valueint : g_cfg.relay_port;
    bool en = cJSON_IsBool(enabled) ? cJSON_IsTrue(enabled) : g_cfg.wan_enabled;
    const char *key_str = (cJSON_IsString(key) && key->valuestring[0] != '\0') ? key->valuestring : NULL;

    if (host_str[0] == '\0' || port_val <= 0 || port_val > 65535) {
        cJSON_Delete(body);
        return reply_error(req, "400 Bad Request", "中继地址/端口不正确");
    }
    esp_err_t err = kvm_config_set_wan(en, host_str, (uint16_t)port_val, key_str);
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return reply_error(req, "500 Internal Server Error", esp_err_to_name(err));
    }
    kvm_wan_reload();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return reply_json(req, o);
}

/* ------------------------------------------------------------------ */
/* 系统                                                                */
/* ------------------------------------------------------------------ */

static esp_err_t h_system(httpd_req_t *req)
{
    cJSON *body = body_json(req);
    if (!body) {
        return reply_error(req, "400 Bad Request", "JSON 解析失败");
    }
    cJSON *action = cJSON_GetObjectItemCaseSensitive(body, "action");
    const char *act = cJSON_IsString(action) ? action->valuestring : "";
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);

    if (strcmp(act, "release") == 0) {
        kvm_input_release_all();
        kvm_net_send_release();
    } else if (strcmp(act, "reboot") == 0) {
        reply_json(req, o);
        cJSON_Delete(body);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
        return ESP_OK;
    } else if (strcmp(act, "forget_wifi") == 0) {
        g_cfg.sta_ssid[0] = '\0';
        g_cfg.sta_pass[0] = '\0';
        kvm_config_save();
        reply_json(req, o);
        cJSON_Delete(body);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
        return ESP_OK;
    } else if (strcmp(act, "factory") == 0) {
        kvm_config_factory_reset();
        reply_json(req, o);
        cJSON_Delete(body);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
        return ESP_OK;
    } else {
        cJSON_Delete(body);
        cJSON_Delete(o);
        return reply_error(req, "400 Bad Request", "未知操作");
    }
    cJSON_Delete(body);
    return reply_json(req, o);
}

/* ------------------------------------------------------------------ */
/* 启动                                                                */
/* ------------------------------------------------------------------ */

void kvm_http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败");
        return;
    }

    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = h_favicon},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = h_wifi_scan},
        {.uri = "/api/wifi/scan", .method = HTTP_POST, .handler = h_wifi_scan_start},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi_set},
        {.uri = "/api/host", .method = HTTP_POST, .handler = h_host_set},
        {.uri = "/api/wan", .method = HTTP_POST, .handler = h_wan_set},
        {.uri = "/api/ble/scan", .method = HTTP_GET, .handler = h_ble_scan_get},
        {.uri = "/api/ble/scan", .method = HTTP_POST, .handler = h_ble_scan_start},
        {.uri = "/api/ble/connect", .method = HTTP_POST, .handler = h_ble_connect},
        {.uri = "/api/ble/disconnect", .method = HTTP_POST, .handler = h_ble_disconnect},
        {.uri = "/api/system", .method = HTTP_POST, .handler = h_system},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
    ESP_LOGI(TAG, "配置页: http://192.168.4.1/  (AP: %s)", kvm_config_ap_ssid());
}
