#include "kvm_captive.h"

#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "kvm_captive";

static uint32_t ap_ip_addr(void)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t info;
    if (ap != NULL && esp_netif_get_ip_info(ap, &info) == ESP_OK) {
        return info.ip.addr;
    }
    return htonl(0xC0A80401); /* 192.168.4.1 */
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket 创建失败");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind 53 失败");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    uint8_t out[600];
    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n < 12) {
            continue;
        }
        /* 只处理标准查询 */
        if ((buf[2] & 0x80) != 0 || ntohs(*(uint16_t *)&buf[4]) < 1) {
            continue;
        }
        int p = 12;
        while (p < n && buf[p] != 0) {
            p += buf[p] + 1;
        }
        if (p + 5 > n) {
            continue;
        }
        uint16_t qtype = (uint16_t)((buf[p + 1] << 8) | buf[p + 2]);
        int q_end = p + 5;

        memcpy(out, buf, 12);
        out[2] = 0x81; /* QR=1, RD=1 */
        out[3] = 0x80; /* RA=1, RCODE=0 */
        uint16_t ancount = (qtype == 1) ? 1 : 0;
        out[6] = (uint8_t)(ancount >> 8);
        out[7] = (uint8_t)ancount;
        out[8] = 0;
        out[9] = 0;
        out[10] = 0;
        out[11] = 0;
        memcpy(out + 12, buf + 12, q_end - 12);
        int len = q_end;
        if (qtype == 1) {
            uint32_t ip = ap_ip_addr();
            uint8_t ans[] = {
                0xC0, 0x0C,             /* name ptr */
                0x00, 0x01, 0x00, 0x01, /* A, IN */
                0x00, 0x00, 0x00, 0x3C, /* TTL 60 */
                0x00, 0x04,
            };
            memcpy(out + len, ans, sizeof(ans));
            len += sizeof(ans);
            memcpy(out + len, &ip, 4);
            len += 4;
        }
        sendto(sock, out, len, 0, (struct sockaddr *)&from, fromlen);
    }
}

void kvm_captive_start(void)
{
    xTaskCreate(dns_task, "kvm_dns", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "强制门户 DNS 已启动（所有域名 -> 配置页）");
}
