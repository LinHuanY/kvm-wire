#include "kvm_wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kvm_config.h"
#include "kvm_net.h"
#include "sdkconfig.h"

static const char *TAG = "kvm_wifi";

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_timer_handle_t s_retry_timer;
static SemaphoreHandle_t s_lock;

static bool s_sta_up;
static char s_ip[16];
static char s_ssid[33];
static int s_rssi = 0;

static bool s_scanning;
static kvm_wifi_ap_t s_scan_aps[KVM_WIFI_MAX_AP];
static int s_scan_count;

static void retry_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "重试连接 WiFi '%s'", g_cfg.sta_ssid);
    esp_wifi_connect();
}

static void scan_done(void)
{
    uint16_t number = KVM_WIFI_MAX_AP;
    wifi_ap_record_t *records = calloc(KVM_WIFI_MAX_AP, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        s_scanning = false;
        return;
    }
    esp_wifi_scan_get_ap_records(&number, records);
    if (number > KVM_WIFI_MAX_AP) {
        number = KVM_WIFI_MAX_AP;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_scan_count = number;
    for (int i = 0; i < number; i++) {
        strlcpy(s_scan_aps[i].ssid, (const char *)records[i].ssid, sizeof(s_scan_aps[i].ssid));
        s_scan_aps[i].rssi = records[i].rssi;
        s_scan_aps[i].authmode = records[i].authmode;
    }
    s_scanning = false;
    xSemaphoreGive(s_lock);
    free(records);
    ESP_LOGI(TAG, "扫描完成，共 %d 个 AP", number);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (g_cfg.sta_ssid[0] != '\0') {
                esp_wifi_connect();
            } else {
                ESP_LOGI(TAG, "尚未配置 WiFi，仅 AP 模式");
            }
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "已连接 AP");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_sta_up = false;
            s_ip[0] = '\0';
            xSemaphoreGive(s_lock);
            kvm_net_on_link_down();
            ESP_LOGW(TAG, "WiFi 断开 (reason=%d)，5s 后重试", ev->reason);
            if (g_cfg.sta_ssid[0] != '\0') {
                esp_timer_stop(s_retry_timer);
                esp_timer_start_once(s_retry_timer, 5000 * 1000);
            }
            break;
        }
        case WIFI_EVENT_SCAN_DONE:
            scan_done();
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_sta_up = true;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        strlcpy(s_ssid, g_cfg.sta_ssid, sizeof(s_ssid));
        xSemaphoreGive(s_lock);
        esp_timer_stop(s_retry_timer);

        wifi_ap_record_t ap = {0};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            s_rssi = ap.rssi;
        }
        ESP_LOGI(TAG, "获得 IP: %s (AP=%s rssi=%d)", s_ip, g_cfg.sta_ssid, s_rssi);
        kvm_net_on_link_up();
    }
}

void kvm_wifi_start(void)
{
    s_lock = xSemaphoreCreateMutex();

    const esp_timer_create_args_t timer_args = {
        .callback = retry_cb,
        .name = "wifi_retry",
    };
    esp_timer_create(&timer_args, &s_retry_timer);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    (void)s_ap_netif;
    (void)s_sta_netif;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, kvm_config_ap_ssid(), sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen((const char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 4;
    if (strlen(kvm_config_ap_password()) >= 8) {
        strlcpy((char *)ap_cfg.ap.password, kvm_config_ap_password(), sizeof(ap_cfg.ap.password));
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    wifi_config_t sta_cfg = {0};
    if (g_cfg.sta_ssid[0] != '\0') {
        strlcpy((char *)sta_cfg.sta.ssid, g_cfg.sta_ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, g_cfg.sta_pass, sizeof(sta_cfg.sta.password));
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 关掉省电，鼠标延迟更稳定 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* 让 AP 客户端把 DNS 指向设备，配合 DNS 劫持实现强制门户 */
    if (s_ap_netif != NULL) {
        esp_netif_ip_info_t ap_ip;
        if (esp_netif_get_ip_info(s_ap_netif, &ap_ip) == ESP_OK) {
            esp_netif_dhcps_stop(s_ap_netif);
            esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                   &ap_ip.ip, sizeof(ap_ip.ip));
            esp_netif_dhcps_start(s_ap_netif);
        }
    }
    ESP_LOGI(TAG, "AP 已启动: SSID=%s  http://192.168.4.1/", kvm_config_ap_ssid());
    if (g_cfg.sta_ssid[0] == '\0') {
        ESP_LOGW(TAG, "未配置家庭 WiFi，仅 AP 模式（可先连 AP 再在网页里配置）");
    }
}

bool kvm_wifi_sta_up(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool up = s_sta_up;
    xSemaphoreGive(s_lock);
    return up;
}

const char *kvm_wifi_sta_ip(void)
{
    return s_ip;
}

const char *kvm_wifi_sta_ssid(void)
{
    return s_ssid;
}

int kvm_wifi_sta_rssi(void)
{
    return s_rssi;
}

void kvm_wifi_apply_sta(void)
{
    if (g_cfg.sta_ssid[0] == '\0') {
        return;
    }
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, g_cfg.sta_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, g_cfg.sta_pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_LOGI(TAG, "应用 WiFi 配置并连接 '%s'", g_cfg.sta_ssid);
    esp_wifi_connect();
}

esp_err_t kvm_wifi_connect_sta(const char *ssid, const char *pass)
{
    esp_err_t err = kvm_config_set_wifi(ssid, pass);
    if (err != ESP_OK) {
        return err;
    }
    kvm_wifi_apply_sta();
    return ESP_OK;
}

esp_err_t kvm_wifi_scan_start(void)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_scanning) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_scanning = true;
    xSemaphoreGive(s_lock);

    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
    };
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        s_scanning = false;
        ESP_LOGW(TAG, "启动扫描失败: %s", esp_err_to_name(err));
    }
    return err;
}

bool kvm_wifi_scanning(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_scanning;
    xSemaphoreGive(s_lock);
    return v;
}

int kvm_wifi_scan_count(void)
{
    if (!s_lock) {
        return 0;
    }
    return s_scan_count;
}

int kvm_wifi_scan_get(kvm_wifi_ap_t *out, int max)
{
    if (!s_lock || out == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_scan_count < max ? s_scan_count : max;
    memcpy(out, s_scan_aps, n * sizeof(kvm_wifi_ap_t));
    xSemaphoreGive(s_lock);
    return n;
}
