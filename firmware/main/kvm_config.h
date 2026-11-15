#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define KVM_MAX_SAVED_DEVS 4
#define KVM_SSID_LEN 33
#define KVM_PASS_LEN 65
#define KVM_IP_LEN 16

typedef struct {
    uint8_t bda[6];
    uint8_t addr_type;
    uint8_t usage; /* KVM_CLASS_* */
    bool enabled;
} kvm_saved_dev_t;

typedef struct {
    char sta_ssid[KVM_SSID_LEN];
    char sta_pass[KVM_PASS_LEN];
    char host_ip[KVM_IP_LEN];
    uint16_t host_port;
    uint64_t token;
    bool auto_connect;
    char ap_pass[KVM_PASS_LEN]; /* 空字符串 = 开放热点 */
    /* 广域网中继 */
    bool wan_enabled;
    char relay_host[64];
    uint16_t relay_port;
    char relay_key[80];
    uint8_t dev_count;
    kvm_saved_dev_t devs[KVM_MAX_SAVED_DEVS];
} kvm_config_t;

extern kvm_config_t g_cfg;

void kvm_config_load(void);
esp_err_t kvm_config_save(void);
void kvm_config_factory_reset(void);

const char *kvm_config_ap_ssid(void);
const char *kvm_config_ap_password(void);

esp_err_t kvm_config_set_wifi(const char *ssid, const char *pass);
esp_err_t kvm_config_set_host(const char *ip, uint16_t port, uint64_t token, bool auto_connect);
esp_err_t kvm_config_set_wan(bool enabled, const char *host, uint16_t port, const char *key);

bool kvm_config_has_device(const uint8_t bda[6]);
esp_err_t kvm_config_add_device(const uint8_t bda[6], uint8_t addr_type, uint8_t usage);
esp_err_t kvm_config_remove_device(const uint8_t bda[6]);
