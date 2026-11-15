/*
 * UDP 链路：把 kvm_input 的事件发到 PC 注入端，并处理 PING/CMD。
 */
#include "kvm_net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kvm_ble.h"
#include "kvm_config.h"
#include "kvm_input.h"
#include "kvm_proto.h"
#include "kvm_wan.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "kvm_net";

#define TX_PERIOD_MS 8
#define HELLO_INTERVAL_MS 10000
#define DISCOVERY_INTERVAL_MS 2000
#define STATUS_INTERVAL_MS 30000
#define KEEPALIVE_INTERVAL_MS 1000

static int s_sock = -1;
static struct sockaddr_in s_peer;
static bool s_peer_set;
static bool s_link_up;
static volatile bool s_status_req;
static volatile bool s_reconfig;

static uint16_t s_seq;
static uint32_t s_dev_ms;
static int64_t s_last_hello_ms;
static int64_t s_last_status_ms;
static int64_t s_last_host_rx_ms;
static bool s_host_seen;

static uint32_t s_tx_count;
static uint32_t s_rx_count;
static uint32_t s_lost_count;
static int64_t s_last_discover_reply_ms;
static int64_t s_last_tx_ms;
static bool s_wan_rx;

static void net_send_to(uint8_t type, const uint8_t *payload, uint8_t len, const struct sockaddr_in *dst)
{
    uint8_t buf[KVM_HDR_LEN + KVM_MAX_PAYLOAD];
    s_dev_ms = (uint32_t)(esp_timer_get_time() / 1000);
    size_t total = kvm_frame_encode(buf, sizeof(buf), type, s_seq++, g_cfg.token, s_dev_ms, payload, len);
    if (total == 0) {
        return;
    }
    /* 广域网中继优先：连上中继时所有 KVM 帧走 TLS */
    if (kvm_wan_connected() && kvm_wan_send(buf, total) == 0) {
        s_last_tx_ms = esp_timer_get_time() / 1000;
        s_tx_count++;
        return;
    }
    if (s_sock < 0) {
        return;
    }
    if (sendto(s_sock, buf, total, 0, (const struct sockaddr *)dst, sizeof(*dst)) < 0) {
        ESP_LOGW(TAG, "sendto 失败: errno=%d", errno);
        return;
    }
    s_last_tx_ms = esp_timer_get_time() / 1000;
    s_tx_count++;
}

static void net_send(uint8_t type, const uint8_t *payload, uint8_t len)
{
    struct sockaddr_in dst;
    if (s_peer_set) {
        dst = s_peer;
    } else {
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_port = htons(g_cfg.host_port);
        dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    }
    net_send_to(type, payload, len, &dst);
}

void kvm_net_send_release(void)
{
    net_send(KVM_MSG_RELEASE, NULL, 0);
}

void kvm_net_request_status(void)
{
    s_status_req = true;
}

void kvm_net_on_link_up(void)
{
    s_link_up = true;
    s_reconfig = true; /* STA 拿到新 IP 后重新解析目标地址 */
}

void kvm_net_on_link_down(void)
{
    s_link_up = false;
    s_peer_set = false;
    s_host_seen = false;
}

static void open_socket(void)
{
    if (s_sock >= 0) {
        return;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "创建 socket 失败: errno=%d", errno);
        return;
    }
    int broadcast = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(CONFIG_KVM_DEVICE_PORT);
    if (bind(s_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "bind 失败: errno=%d", errno);
        close(s_sock);
        s_sock = -1;
        return;
    }

    if (g_cfg.host_ip[0] != '\0') {
        memset(&s_peer, 0, sizeof(s_peer));
        s_peer.sin_family = AF_INET;
        s_peer.sin_port = htons(g_cfg.host_port);
        if (inet_pton(AF_INET, g_cfg.host_ip, &s_peer.sin_addr) == 1) {
            s_peer_set = true;
            ESP_LOGI(TAG, "目标 PC: %s:%u", g_cfg.host_ip, g_cfg.host_port);
        } else {
            ESP_LOGE(TAG, "host_ip 不合法: %s", g_cfg.host_ip);
        }
    } else {
        ESP_LOGI(TAG, "未指定 PC IP，使用广播自动发现（端口 %u）", g_cfg.host_port);
        s_peer_set = false;
    }
}

static void close_socket(void)
{
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
}

static uint8_t build_hello(uint8_t *payload)
{
    payload[0] = KVM_VERSION;
    payload[1] = 1; /* fw major */
    payload[2] = 0; /* fw minor */
    payload[3] = 0x07;
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    memcpy(payload + 4, mac, 6);
    const char *name = "esp32s3-kvm";
    size_t name_len = strlen(name);
    if (name_len > 54) {
        name_len = 54;
    }
    memcpy(payload + 10, name, name_len);
    return (uint8_t)(10 + name_len);
}

static void send_hello(void)
{
    uint8_t payload[64];
    uint8_t len = build_hello(payload);
    net_send(KVM_MSG_HELLO, payload, len);
    s_last_hello_ms = esp_timer_get_time() / 1000;
}

/* 回应 PC 的 DISCOVER：只带设备身份，不改变当前锁定的主机 */
static void reply_hello_to(const struct sockaddr_in *dst)
{
    uint8_t payload[64];
    uint8_t len = build_hello(payload);
    net_send_to(KVM_MSG_HELLO, payload, len, dst);
    char ip[16] = "?";
    inet_ntop(AF_INET, &dst->sin_addr, ip, sizeof(ip));
    ESP_LOGI(TAG, "回应发现请求 -> %s:%u", ip, ntohs(dst->sin_port));
}

static void send_status(void)
{
    kvm_ble_conn_t conns[KVM_BLE_MAX_CONNS];
    int n = kvm_ble_conn_get(conns, KVM_BLE_MAX_CONNS);
    uint8_t payload[1 + 3 * KVM_BLE_MAX_CONNS];
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (!conns[i].connected) {
            continue;
        }
        payload[1 + count * 3 + 0] = conns[i].usage;
        payload[1 + count * 3 + 1] = conns[i].battery < 0 ? 255 : (uint8_t)conns[i].battery;
        payload[1 + count * 3 + 2] = (uint8_t)conns[i].rssi;
        count++;
    }
    payload[0] = (uint8_t)count;
    net_send(KVM_MSG_STATUS, payload, (uint8_t)(1 + count * 3));
    s_last_status_ms = esp_timer_get_time() / 1000;
    s_status_req = false;
}

static void handle_packet(const uint8_t *data, size_t len, const struct sockaddr_in *from)
{
    kvm_frame_t frame;
    if (!kvm_frame_decode(data, len, &frame)) {
        return;
    }
    /* 发现请求不需要 token；回应 HELLO 让 PC 自动找到设备 */
    if (frame.type == KVM_MSG_DISCOVER) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now - s_last_discover_reply_ms >= 500) {
            s_last_discover_reply_ms = now;
            reply_hello_to(from);
        }
        return;
    }
    if (g_cfg.token != 0 && frame.token != g_cfg.token) {
        return;
    }
    s_rx_count++;
    s_last_host_rx_ms = esp_timer_get_time() / 1000;

    if (!s_host_seen && !s_wan_rx) {
        s_host_seen = true;
        char ip[16] = "?";
        inet_ntop(AF_INET, &from->sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "收到 PC 数据包，来自 %s:%u", ip, ntohs(from->sin_port));
        /* 自动发现：没有配置 host_ip 时，记住 PC 的地址 */
        if (g_cfg.host_ip[0] == '\0') {
            s_peer = *from;
            s_peer_set = true;
            ESP_LOGI(TAG, "已锁定 PC 地址 %s:%u", ip, ntohs(from->sin_port));
        }
    }

    switch (frame.type) {
    case KVM_MSG_PING: {
        if (frame.plen >= 4) {
            uint32_t host_ms = kvm_get_u32(frame.payload);
            uint8_t payload[8];
            kvm_put_u32(payload, host_ms);
            kvm_put_u32(payload + 4, (uint32_t)(esp_timer_get_time() / 1000));
            /* 回到发送者（UDP 或中继） */
            net_send_to(KVM_MSG_PONG, payload, sizeof(payload), from);
        }
        break;
    }
    case KVM_MSG_CMD: {
        if (frame.plen >= 2) {
            uint8_t cmd = frame.payload[0];
            if (cmd == KVM_CMD_RESYNC) {
                kvm_input_resync();
                ESP_LOGI(TAG, "收到 RESYNC，重发按键状态");
            } else if (cmd == KVM_CMD_RELEASE_ALL) {
                kvm_input_release_all();
                s_status_req = true;
            } else if (cmd == KVM_CMD_SCAN) {
                kvm_ble_scan_start(8000);
            }
        }
        break;
    }
    default:
        break;
    }
}

void kvm_net_handle_frame(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    struct sockaddr_in from;
    memset(&from, 0, sizeof(from));
    from.sin_family = AF_INET;
    from.sin_port = htons(g_cfg.relay_port);
    inet_pton(AF_INET, g_cfg.relay_host, &from.sin_addr);
    s_wan_rx = true;
    handle_packet(data, len, &from);
    s_wan_rx = false;
}

static void drain_rx(void)
{
    uint8_t buf[256];
    for (int i = 0; i < 16; i++) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGW(TAG, "recvfrom 失败: errno=%d", errno);
            }
            return;
        }
        handle_packet(buf, (size_t)n, &from);
    }
}

static void flush_input(void)
{
    uint8_t payload[KVM_MAX_PAYLOAD];

    uint8_t mods = 0;
    uint16_t usages[8];
    uint8_t count = 0;
    if (kvm_input_take_keyboard(&mods, usages, &count)) {
        uint8_t plen = kvm_key_payload(payload, KVM_PAGE_KEYBOARD, mods, usages, count);
        net_send(KVM_MSG_KEY, payload, plen);
    }

    uint16_t consumer = 0;
    if (kvm_input_take_consumer(&consumer)) {
        uint16_t one = consumer;
        uint8_t plen = kvm_key_payload(payload, KVM_PAGE_CONSUMER, 0, &one, 1);
        net_send(KVM_MSG_KEY, payload, plen);
        plen = kvm_key_payload(payload, KVM_PAGE_CONSUMER, 0, &one, 0);
        net_send(KVM_MSG_KEY, payload, plen);
    }

    uint8_t buttons = 0;
    int16_t dx = 0, dy = 0, wheel = 0, pan = 0;
    if (kvm_input_take_mouse(&buttons, &dx, &dy, &wheel, &pan)) {
        uint8_t plen = kvm_mouse_payload(payload, buttons, false, dx, dy, wheel, pan);
        net_send(KVM_MSG_MOUSE, payload, plen);
    }
}

static void net_task(void *arg)
{
    (void)arg;
    int64_t last_discovery = 0;

    while (1) {
        if (s_reconfig) {
            s_reconfig = false;
            s_peer_set = false;
            close_socket();
        }
        open_socket();
        if (s_sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        drain_rx();
        flush_input();

        int64_t now = esp_timer_get_time() / 1000;
        if (s_status_req || now - s_last_status_ms >= STATUS_INTERVAL_MS) {
            send_status();
        }
        if (!s_peer_set) {
            if (now - last_discovery >= DISCOVERY_INTERVAL_MS) {
                last_discovery = now;
                send_hello();
            }
        } else if (now - s_last_hello_ms >= HELLO_INTERVAL_MS) {
            send_hello();
        } else if (now - s_last_tx_ms >= KEEPALIVE_INTERVAL_MS) {
            /* 没有键鼠事件时也保持链路活跃，避免 PC 端看门狗误释放按键 */
            net_send(KVM_MSG_KEEPALIVE, NULL, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(TX_PERIOD_MS));
    }
}

void kvm_net_start(void)
{
    /* 立即绑定 8766：即使只开 AP（没有连路由器）PC 也能连上 */
    open_socket();
    kvm_wan_set_frame_cb(kvm_net_handle_frame);
    xTaskCreate(net_task, "kvm_net", 5120, NULL, 6, NULL);
}

void kvm_net_apply_config(void)
{
    /* 只打标记，由网络任务自己关闭/重建 socket，避免跨任务关闭 fd */
    s_reconfig = true;
    s_host_seen = false;
}

bool kvm_net_host_online(void)
{
    if (!s_host_seen) {
        return false;
    }
    return (esp_timer_get_time() / 1000 - s_last_host_rx_ms) < 6000;
}

int kvm_net_host_idle_ms(void)
{
    if (!s_host_seen) {
        return -1;
    }
    int64_t idle = esp_timer_get_time() / 1000 - s_last_host_rx_ms;
    return (int)(idle > 1000000 ? 1000000 : idle);
}

uint32_t kvm_net_tx_count(void)
{
    return s_tx_count;
}

uint32_t kvm_net_rx_count(void)
{
    return s_rx_count;
}

uint32_t kvm_net_lost_count(void)
{
    return s_lost_count;
}
