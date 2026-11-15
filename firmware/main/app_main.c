/*
 * ESP32-S3 蓝牙键鼠 -> 局域网 -> PC 注入端
 *
 * 启动流程：
 *   1. 读 NVS 配置
 *   2. 开 AP（配置热点）+ STA（连接局域网）
 *   3. 启动 BLE HID Host
 *   4. 启动 UDP 链路与配置网页
 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kvm_ble.h"
#include "kvm_config.h"
#include "kvm_http.h"
#include "kvm_input.h"
#include "kvm_captive.h"
#include "kvm_net.h"
#include "kvm_wan.h"
#include "kvm_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "kvm";

#define LED_GPIO CONFIG_KVM_STATUS_LED_GPIO
#define LED_ACTIVE_LOW CONFIG_KVM_LED_ACTIVE_LOW
#define BUTTON_GPIO CONFIG_KVM_BUTTON_GPIO

static void led_write(bool on)
{
#if LED_GPIO >= 0
#if LED_ACTIVE_LOW
    gpio_set_level(LED_GPIO, on ? 0 : 1);
#else
    gpio_set_level(LED_GPIO, on ? 1 : 0);
#endif
#else
    (void)on;
#endif
}

static void led_init(void)
{
#if LED_GPIO >= 0
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    led_write(false);
#endif
}

static void led_task(void *arg)
{
    (void)arg;
    int64_t start = esp_timer_get_time() / 1000;
    while (1) {
        int64_t now = esp_timer_get_time() / 1000;
        int64_t t = now - start;
        if (kvm_wifi_sta_up() && kvm_ble_any_connected()) {
            led_write(true);
        } else if (kvm_wifi_sta_up()) {
            led_write(((t / 500) % 2) == 0);
        } else {
            /* AP 模式：每 2 秒闪一下 */
            led_write((t % 2000) < 80);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void button_task(void *arg)
{
    (void)arg;
#if BUTTON_GPIO >= 0
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    int pressed_ms = 0;
    while (1) {
        if (gpio_get_level(BUTTON_GPIO) == 0) {
            pressed_ms += 100;
            if (pressed_ms == 1000) {
                ESP_LOGW(TAG, "按住 BOOT 键 5 秒将恢复出厂设置…");
            }
            if (pressed_ms >= 5000) {
                ESP_LOGW(TAG, "恢复出厂设置并重启");
                kvm_config_factory_reset();
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        } else {
            pressed_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
#else
    vTaskDelete(NULL);
#endif
}

static void status_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "状态: WiFi=%s ip=%s BLE连接=%d TX=%lu RX=%lu 堆=%lu",
                 kvm_wifi_sta_up() ? "up" : "down", kvm_wifi_sta_ip(), kvm_ble_conn_count(),
                 (unsigned long)kvm_net_tx_count(), (unsigned long)kvm_net_rx_count(),
                 (unsigned long)esp_get_free_heap_size());
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "==== ESP32-S3 KVM Bridge (%s) ====", CONFIG_IDF_TARGET);
    kvm_config_load();

    kvm_input_init();
    led_init();

    kvm_wifi_start();
    kvm_captive_start();
    kvm_ble_start();
    kvm_net_start();
    kvm_wan_start();
    kvm_http_start();

    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
    xTaskCreate(button_task, "button", 2560, NULL, 2, NULL);
    xTaskCreate(status_task, "status", 3072, NULL, 1, NULL);

    ESP_LOGI(TAG, "配置热点: %s   打开 http://192.168.4.1/", kvm_config_ap_ssid());
}

