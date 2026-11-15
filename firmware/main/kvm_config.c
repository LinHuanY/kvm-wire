#include "kvm_config.h"

#include "esp_random.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "kvm_cfg";
static const char *NVS_NS = "kvm";

kvm_config_t g_cfg;
static char s_ap_ssid[KVM_SSID_LEN];

static uint64_t parse_token(const char *text)
{
    if (text == NULL) {
        return 0;
    }
    const char *p = text;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 16);
    if (end == p) {
        return 0;
    }
    return (uint64_t)v;
}

void kvm_config_load(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));

    strlcpy(g_cfg.sta_ssid, CONFIG_KVM_DEFAULT_STA_SSID, sizeof(g_cfg.sta_ssid));
    strlcpy(g_cfg.sta_pass, CONFIG_KVM_DEFAULT_STA_PASS, sizeof(g_cfg.sta_pass));
    strlcpy(g_cfg.host_ip, CONFIG_KVM_DEFAULT_HOST_IP, sizeof(g_cfg.host_ip));
    g_cfg.host_port = CONFIG_KVM_DEFAULT_HOST_PORT;
    g_cfg.token = parse_token(CONFIG_KVM_DEFAULT_TOKEN);
    g_cfg.auto_connect = true;
    /* 默认开放热点，避免手机/电脑因 WPA2 兼容问题连不上 */
    g_cfg.ap_pass[0] = '\0';
    /* 广域网中继默认值（密钥需在网页里填） */
    g_cfg.wan_enabled = false;
    strlcpy(g_cfg.relay_host, CONFIG_KVM_DEFAULT_RELAY_HOST, sizeof(g_cfg.relay_host));
    g_cfg.relay_port = CONFIG_KVM_DEFAULT_RELAY_PORT;
    strlcpy(g_cfg.relay_key, CONFIG_KVM_DEFAULT_RELAY_KEY, sizeof(g_cfg.relay_key));
    g_cfg.e2e_key[0] = '\0';
    g_cfg.web_pass[0] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "无已保存配置，使用 Kconfig 默认值");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open 失败: %s", esp_err_to_name(err));
        return;
    }

    size_t len = sizeof(g_cfg.sta_ssid);
    if (nvs_get_str(h, "ssid", g_cfg.sta_ssid, &len) != ESP_OK) {
        /* 忽略 */
    }
    len = sizeof(g_cfg.sta_pass);
    if (nvs_get_str(h, "pass", g_cfg.sta_pass, &len) != ESP_OK) {
        /* 忽略 */
    }
    len = sizeof(g_cfg.host_ip);
    if (nvs_get_str(h, "hip", g_cfg.host_ip, &len) != ESP_OK) {
        /* 忽略 */
    }
    uint16_t port = 0;
    if (nvs_get_u16(h, "hport", &port) == ESP_OK && port) {
        g_cfg.host_port = port;
    }
    uint64_t token = 0;
    if (nvs_get_u64(h, "token", &token) == ESP_OK) {
        g_cfg.token = token;
    }
    uint8_t ac = 0;
    if (nvs_get_u8(h, "ac", &ac) == ESP_OK) {
        g_cfg.auto_connect = ac != 0;
    }
    len = sizeof(g_cfg.devs);
    if (nvs_get_blob(h, "devs", g_cfg.devs, &len) != ESP_OK) {
        /* 忽略 */
    } else {
        g_cfg.dev_count = (uint8_t)(len / sizeof(kvm_saved_dev_t));
        if (g_cfg.dev_count > KVM_MAX_SAVED_DEVS) {
            g_cfg.dev_count = KVM_MAX_SAVED_DEVS;
        }
    }
    len = sizeof(g_cfg.ap_pass);
    if (nvs_get_str(h, "appass", g_cfg.ap_pass, &len) != ESP_OK) {
        g_cfg.ap_pass[0] = '\0';
    }
    uint8_t wan_en = 0;
    if (nvs_get_u8(h, "wan_en", &wan_en) == ESP_OK) {
        g_cfg.wan_enabled = wan_en != 0;
    }
    len = sizeof(g_cfg.relay_host);
    if (nvs_get_str(h, "rhost", g_cfg.relay_host, &len) != ESP_OK) {
        /* 保留默认值 */
    }
    uint16_t rport = 0;
    if (nvs_get_u16(h, "rport", &rport) == ESP_OK && rport) {
        g_cfg.relay_port = rport;
    }
    len = sizeof(g_cfg.relay_key);
    if (nvs_get_str(h, "rkey", g_cfg.relay_key, &len) != ESP_OK) {
        /* 保留默认值 */
    }
    len = sizeof(g_cfg.e2e_key);
    if (nvs_get_str(h, "e2ekey", g_cfg.e2e_key, &len) != ESP_OK) {
        g_cfg.e2e_key[0] = '\0';
    }
    len = sizeof(g_cfg.web_pass);
    if (nvs_get_str(h, "webpass", g_cfg.web_pass, &len) != ESP_OK) {
        g_cfg.web_pass[0] = '\0';
    }
    nvs_close(h);

    ESP_LOGI(TAG, "配置: sta_ssid='%s' host=%s:%u token=%016llx devs=%u",
             g_cfg.sta_ssid, g_cfg.host_ip[0] ? g_cfg.host_ip : "(auto)", g_cfg.host_port,
             (unsigned long long)g_cfg.token, g_cfg.dev_count);
}

esp_err_t kvm_config_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open 失败: %s", esp_err_to_name(err));
        return err;
    }
    nvs_set_str(h, "ssid", g_cfg.sta_ssid);
    nvs_set_str(h, "pass", g_cfg.sta_pass);
    nvs_set_str(h, "hip", g_cfg.host_ip);
    nvs_set_u16(h, "hport", g_cfg.host_port);
    nvs_set_u64(h, "token", g_cfg.token);
    nvs_set_u8(h, "ac", g_cfg.auto_connect ? 1 : 0);
    nvs_set_blob(h, "devs", g_cfg.devs, g_cfg.dev_count * sizeof(kvm_saved_dev_t));
    nvs_set_str(h, "appass", g_cfg.ap_pass);
    nvs_set_u8(h, "wan_en", g_cfg.wan_enabled ? 1 : 0);
    nvs_set_str(h, "rhost", g_cfg.relay_host);
    nvs_set_u16(h, "rport", g_cfg.relay_port);
    nvs_set_str(h, "rkey", g_cfg.relay_key);
    nvs_set_str(h, "e2ekey", g_cfg.e2e_key);
    nvs_set_str(h, "webpass", g_cfg.web_pass);
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "配置已保存");
    }
    return err;
}

void kvm_config_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "已恢复出厂设置");
}

const char *kvm_config_ap_ssid(void)
{
    if (s_ap_ssid[0] == '\0') {
        uint8_t mac[6] = {0};
        if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
            memset(mac, 0x42, sizeof(mac));
        }
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X", CONFIG_KVM_AP_SSID_PREFIX, mac[4], mac[5]);
    }
    return s_ap_ssid;
}

const char *kvm_config_ap_password(void)
{
    return g_cfg.ap_pass;
}

esp_err_t kvm_config_set_wifi(const char *ssid, const char *pass)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(g_cfg.sta_ssid, ssid, sizeof(g_cfg.sta_ssid));
    if (pass != NULL) {
        strlcpy(g_cfg.sta_pass, pass, sizeof(g_cfg.sta_pass));
    }
    return kvm_config_save();
}

esp_err_t kvm_config_set_host(const char *ip, uint16_t port, uint64_t token, bool auto_connect)
{
    if (ip != NULL) {
        strlcpy(g_cfg.host_ip, ip, sizeof(g_cfg.host_ip));
    }
    if (port) {
        g_cfg.host_port = port;
    }
    if (token) {
        g_cfg.token = token;
    }
    g_cfg.auto_connect = auto_connect;
    return kvm_config_save();
}

esp_err_t kvm_config_set_wan(bool enabled, const char *host, uint16_t port, const char *key)
{
    g_cfg.wan_enabled = enabled;
    if (host != NULL) {
        strlcpy(g_cfg.relay_host, host, sizeof(g_cfg.relay_host));
    }
    if (port) {
        g_cfg.relay_port = port;
    }
    if (key != NULL) {
        strlcpy(g_cfg.relay_key, key, sizeof(g_cfg.relay_key));
    }
    return kvm_config_save();
}

bool kvm_config_has_device(const uint8_t bda[6]){
    for (uint8_t i = 0; i < g_cfg.dev_count; i++) {
        if (memcmp(g_cfg.devs[i].bda, bda, 6) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t kvm_config_add_device(const uint8_t bda[6], uint8_t addr_type, uint8_t usage)
{
    for (uint8_t i = 0; i < g_cfg.dev_count; i++) {
        if (memcmp(g_cfg.devs[i].bda, bda, 6) == 0) {
            g_cfg.devs[i].addr_type = addr_type;
            g_cfg.devs[i].usage = usage;
            g_cfg.devs[i].enabled = true;
            return kvm_config_save();
        }
    }
    if (g_cfg.dev_count >= KVM_MAX_SAVED_DEVS) {
        return ESP_ERR_NO_MEM;
    }
    kvm_saved_dev_t *d = &g_cfg.devs[g_cfg.dev_count++];
    memcpy(d->bda, bda, 6);
    d->addr_type = addr_type;
    d->usage = usage;
    d->enabled = true;
    return kvm_config_save();
}

esp_err_t kvm_config_remove_device(const uint8_t bda[6])
{
    for (uint8_t i = 0; i < g_cfg.dev_count; i++) {
        if (memcmp(g_cfg.devs[i].bda, bda, 6) == 0) {
            if (i + 1 < g_cfg.dev_count) {
                memmove(&g_cfg.devs[i], &g_cfg.devs[i + 1], (g_cfg.dev_count - i - 1) * sizeof(kvm_saved_dev_t));
            }
            g_cfg.dev_count--;
            return kvm_config_save();
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t kvm_config_ensure_e2e_key(void)
{
    if (g_cfg.e2e_key[0] != '\0') {
        return ESP_OK;
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        uint8_t b = (uint8_t)esp_random();
        g_cfg.e2e_key[i * 2] = hex[b >> 4];
        g_cfg.e2e_key[i * 2 + 1] = hex[b & 0x0F];
    }
    g_cfg.e2e_key[32] = '\0';
    ESP_LOGI("kvm_cfg", "已生成设备配对码");
    return kvm_config_save();
}
