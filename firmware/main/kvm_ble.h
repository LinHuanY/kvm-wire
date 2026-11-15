#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define KVM_BLE_MAX_SCAN 64
#define KVM_BLE_MAX_CONNS 4

typedef struct {
    uint8_t bda[6];
    uint8_t addr_type;
    int8_t rssi;
    uint16_t appearance;
    uint8_t usage; /* KVM_CLASS_*，0=未知 */
    bool hid;      /* 广播里带 HID 服务或键鼠外观 */
    char name[33];
} kvm_ble_scan_dev_t;

typedef struct {
    uint8_t bda[6];
    uint8_t addr_type;
    uint8_t usage;
    int8_t battery; /* -1 = 未知 */
    int8_t rssi;
    bool connected;
    char name[33];
} kvm_ble_conn_t;

void kvm_ble_start(void);

esp_err_t kvm_ble_scan_start(uint32_t duration_ms);
esp_err_t kvm_ble_scan_start_ex(uint32_t duration_ms, bool whitelist);
bool kvm_ble_scanning(void);
int kvm_ble_scan_get(kvm_ble_scan_dev_t *out, int max);

esp_err_t kvm_ble_connect(const uint8_t bda[6], uint8_t addr_type);
esp_err_t kvm_ble_disconnect(const uint8_t bda[6]);

int kvm_ble_conn_get(kvm_ble_conn_t *out, int max);
int kvm_ble_conn_count(void);
bool kvm_ble_any_connected(void);
