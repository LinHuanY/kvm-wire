#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define KVM_WIFI_MAX_AP 20

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
} kvm_wifi_ap_t;

void kvm_wifi_start(void);

bool kvm_wifi_sta_up(void);
const char *kvm_wifi_sta_ip(void);
const char *kvm_wifi_sta_ssid(void);
int kvm_wifi_sta_rssi(void);

/* 保存并连接新的 AP，失败会继续保持重连 */
esp_err_t kvm_wifi_connect_sta(const char *ssid, const char *pass);

/* 用当前 g_cfg 里的 STA 配置重连（不写 NVS），供网页延迟调用 */
void kvm_wifi_apply_sta(void);

/* 异步扫描，结果通过下面的函数读取 */
esp_err_t kvm_wifi_scan_start(void);
bool kvm_wifi_scanning(void);
int kvm_wifi_scan_count(void);
int kvm_wifi_scan_get(kvm_wifi_ap_t *out, int max);
