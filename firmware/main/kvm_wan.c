/*
 * 广域网中继客户端：TLS 连到 relay 服务器，注册设备并把 KVM 帧双向转发。
 * 协议：4 字节大端长度 + 1 字节类型 + 负载（与 host/wan_relay.py 一致）。
 */
#include "kvm_wan.h"

#include <string.h>
#include <sys/select.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kvm_config.h"
#include "kvm_net.h"
#include "kvm_proto.h"
#include "kvm_wifi.h"
#include "lwip/sockets.h"
#include "netinet/tcp.h"

static const char *TAG = "kvm_wan";

#define T_HELLO_DEVICE 1
#define T_HELLO_CLIENT 2
#define T_HELLO_OK 3
#define T_DEVICE_LIST 4
#define T_CONNECT 5
#define T_PEER_READY 6
#define T_DATA 7
#define T_PING 8
#define T_PONG 9
#define T_ERROR 10
#define T_PEER_GONE 11
#define T_BYE 12

#define MAX_FRAME 8192
#define RELAY_PING_MS 20000

extern const uint8_t relay_ca_pem_start[] asm("_binary_relay_ca_pem_start");
extern const uint8_t relay_ca_pem_end[] asm("_binary_relay_ca_pem_end");

static esp_tls_t *s_tls;
static SemaphoreHandle_t s_tx_lock;
static QueueHandle_t s_tx_q;
static volatile bool s_connected;

typedef struct {
    uint16_t len;
    uint8_t data[320];
} wan_tx_item_t;
static volatile bool s_reload;
static volatile bool s_running;
static char s_status[48] = "off";
static kvm_wan_frame_cb_t s_frame_cb;

bool kvm_wan_connected(void)
{
    return s_connected;
}

void kvm_wan_set_frame_cb(kvm_wan_frame_cb_t cb)
{
    s_frame_cb = cb;
}

void kvm_wan_reload(void)
{
    s_reload = true;
}

const char *kvm_wan_status_text(void)
{
    return s_status;
}

/* ------------------------------------------------------------------ */
/* TLS 读写                                                            */
/* ------------------------------------------------------------------ */

static int tls_write_all(const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = esp_tls_conn_write(s_tls, buf + sent, len - sent);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* 返回 >0 读到的字节数，0 表示超时，-1 表示断开 */
static int tls_read_exact(uint8_t *buf, size_t len, int timeout_ms)
{
    int fd = -1;
    if (esp_tls_get_conn_sockfd(s_tls, &fd) != ESP_OK || fd < 0) {
        return -1;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = 5 * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int64_t deadline = esp_timer_get_time() / 1000 + timeout_ms;
    size_t got = 0;
    while (got < len) {
        int n = esp_tls_conn_read(s_tls, buf + got, len - got);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n == 0) {
            /* 对端关闭 */
            return -1;
        }
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            if (esp_timer_get_time() / 1000 >= deadline) {
                return got == 0 ? 0 : -1;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        /* 超时（mbedtls 可能返回 -1/errno=EAGAIN） */
        if (esp_timer_get_time() / 1000 < deadline) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        return got == 0 ? 0 : -1;
    }
    return (int)got;
}

static int relay_send(uint8_t type, const uint8_t *payload, size_t len)
{
    if (s_tls == NULL || !s_connected) {
        return -1;
    }
    uint8_t hdr[5];
    uint32_t total = (uint32_t)len + 1;
    hdr[0] = (uint8_t)(total >> 24);
    hdr[1] = (uint8_t)(total >> 16);
    hdr[2] = (uint8_t)(total >> 8);
    hdr[3] = (uint8_t)total;
    hdr[4] = type;
    int rc = -1;
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (tls_write_all(hdr, sizeof(hdr)) == 0 &&
            (len == 0 || tls_write_all(payload, len) == 0)) {
            rc = 0;
        }
        xSemaphoreGive(s_tx_lock);
    }
    return rc;
}

static void relay_send_json(uint8_t type, cJSON *obj)
{
    char *text = cJSON_PrintUnformatted(obj);
    if (text) {
        relay_send(type, (const uint8_t *)text, strlen(text));
        cJSON_free(text);
    }
}

int kvm_wan_send(const uint8_t *data, size_t len)
{
    if (!s_connected || data == NULL || len == 0 || s_tx_q == NULL) {
        return -1;
    }
    if (len > 320) {
        return -1;
    }
    wan_tx_item_t item;
    item.len = (uint16_t)len;
    memcpy(item.data, data, len);
    if (xQueueSend(s_tx_q, &item, 0) != pdTRUE) {
        return -1; /* 队列满，丢一帧（下一帧会带上最新状态） */
    }
    return 0;
}

static void drain_tx(void)
{
    wan_tx_item_t item;
    int budget = 24;
    while (budget-- > 0 && s_tx_q != NULL && xQueueReceive(s_tx_q, &item, 0) == pdTRUE) {
        relay_send(T_DATA, item.data, item.len);
    }
}

/* ------------------------------------------------------------------ */
/* 连接与主循环                                                        */
/* ------------------------------------------------------------------ */

static void device_id_hex(char out[13])
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static esp_tls_t *relay_connect(const char *host, uint16_t port)
{
    esp_tls_cfg_t cfg = {
        .cacert_buf = relay_ca_pem_start,
        .cacert_bytes = (unsigned int)(relay_ca_pem_end - relay_ca_pem_start),
        .timeout_ms = 8000,
    };
    esp_tls_t *tls = esp_tls_init();
    if (tls == NULL) {
        return NULL;
    }
    int ret = esp_tls_conn_new_sync(host, strlen(host), port, &cfg, tls);
    if (ret != 1) {
        ESP_LOGW(TAG, "TLS 连接 %s:%u 失败 (%d)", host, port, ret);
        esp_tls_conn_destroy(tls);
        return NULL;
    }
    int fd = -1;
    if (esp_tls_get_conn_sockfd(tls, &fd) == ESP_OK && fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return tls;
}

static bool relay_handshake(void)
{
    char dev_id[13];
    device_id_hex(dev_id);
    cJSON *hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "key", g_cfg.relay_key);
    cJSON_AddStringToObject(hello, "device_id", dev_id);
    cJSON_AddStringToObject(hello, "name", "esp32s3-kvm");
    bool ok = false;
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        char *text = cJSON_PrintUnformatted(hello);
        if (text) {
            size_t len = strlen(text);
            uint8_t hdr[5] = {0, 0, 0, (uint8_t)(len + 1), T_HELLO_DEVICE};
            if (tls_write_all(hdr, sizeof(hdr)) == 0 && tls_write_all((uint8_t *)text, len) == 0) {
                ok = true;
            }
            cJSON_free(text);
        }
        xSemaphoreGive(s_tx_lock);
    }
    cJSON_Delete(hello);
    if (!ok) {
        return false;
    }
    /* 等 HELLO_OK */
    uint8_t hdr[4];
    int rh = tls_read_exact(hdr, 4, 8000);
    if (rh != 4) {
        ESP_LOGW(TAG, "等待 HELLO_OK 失败: read=%d", rh);
        return false;
    }
    uint32_t total = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
    if (total < 1 || total > MAX_FRAME) {
        return false;
    }
    uint8_t buf[MAX_FRAME];
    if (tls_read_exact(buf, total, 8000) != (int)total) {
        return false;
    }
    uint8_t type = buf[0];
    if (type != T_HELLO_OK) {
        ESP_LOGW(TAG, "中继拒绝注册: type=%u %.*s", type, (int)(total - 1), (char *)buf + 1);
        return false;
    }
    ESP_LOGI(TAG, "中继注册成功 device_id=%s", dev_id);
    return true;
}

static void wan_task(void *arg)
{
    (void)arg;
    char host[64];
    uint16_t port;
    int backoff = 3;
    int64_t last_ping = 0;

    while (1) {
        if (!g_cfg.wan_enabled) {
            strlcpy(s_status, "disabled", sizeof(s_status));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (g_cfg.relay_key[0] == '\0') {
            strlcpy(s_status, "no key", sizeof(s_status));
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (!kvm_wifi_sta_up()) {
            strlcpy(s_status, "waiting wifi", sizeof(s_status));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        strlcpy(host, g_cfg.relay_host, sizeof(host));
        port = g_cfg.relay_port;
        strlcpy(s_status, "connecting", sizeof(s_status));
        ESP_LOGI(TAG, "连接中继 %s:%u ...", host, port);
        s_tls = relay_connect(host, port);
        if (s_tls == NULL) {
            strlcpy(s_status, "connect fail", sizeof(s_status));
            vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
            if (backoff < 30) {
                backoff += 5;
            }
            continue;
        }
        if (!relay_handshake()) {
            strlcpy(s_status, "auth fail", sizeof(s_status));
            esp_tls_conn_destroy(s_tls);
            s_tls = NULL;
            vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
            continue;
        }

        s_connected = true;
        s_reload = false;
        strlcpy(s_status, "connected", sizeof(s_status));
        backoff = 3;
        last_ping = esp_timer_get_time() / 1000;
        ESP_LOGI(TAG, "广域网已就绪，等待 PC 连接");

        while (1) {
            if (s_reload || !g_cfg.wan_enabled || !kvm_wifi_sta_up()) {
                break;
            }
            drain_tx();
            uint8_t hdr[4];
            int r = tls_read_exact(hdr, 4, 4);
            if (r < 0) {
                ESP_LOGW(TAG, "中继连接断开");
                break;
            }
            int64_t now = esp_timer_get_time() / 1000;
            if (now - last_ping > RELAY_PING_MS) {
                last_ping = now;
                relay_send(T_PING, NULL, 0);
            }
            if (r == 0) {
                continue; /* 超时，继续等 */
            }
            uint32_t total = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
            if (total < 1 || total > MAX_FRAME) {
                ESP_LOGW(TAG, "非法中继帧长度 %u", total);
                break;
            }
            uint8_t buf[MAX_FRAME];
            if (tls_read_exact(buf, total, 3000) != (int)total) {
                ESP_LOGW(TAG, "中继帧读取不完整");
                break;
            }
            uint8_t type = buf[0];
            switch (type) {
            case T_PEER_READY:
                ESP_LOGI(TAG, "PC 已通过中继连入");
                strlcpy(s_status, "paired", sizeof(s_status));
                break;
            case T_DATA:
                if (s_frame_cb && total > 1) {
                    s_frame_cb(buf + 1, total - 1);
                }
                break;
            case T_PING:
                relay_send(T_PONG, NULL, 0);
                break;
            case T_PEER_GONE:
                ESP_LOGW(TAG, "PC 已断开");
                strlcpy(s_status, "connected", sizeof(s_status));
                break;
            case T_ERROR:
                ESP_LOGW(TAG, "中继错误: %.*s", (int)(total - 1), (char *)buf + 1);
                break;
            case T_BYE:
                break;
            default:
                break;
            }
        }

        s_connected = false;
        strlcpy(s_status, "disconnected", sizeof(s_status));
        if (s_tls) {
            esp_tls_conn_destroy(s_tls);
            s_tls = NULL;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void kvm_wan_start(void)
{
    s_tx_lock = xSemaphoreCreateMutex();
    /* 32 帧 × 320B，保证 BLE 回调/网络任务与 TLS 写互不阻塞 */
    s_tx_q = xQueueCreate(32, sizeof(wan_tx_item_t));
    s_running = true;
    xTaskCreate(wan_task, "kvm_wan", 16384, NULL, 4, NULL);
}


