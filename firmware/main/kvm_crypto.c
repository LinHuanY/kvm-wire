#include "kvm_crypto.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"

static const char *TAG = "kvm_crypto";

#define MAGIC "KVE1"
#define MAGIC_LEN 4
#define NONCE_LEN 12
#define TAG_LEN 16
#define AAD "kvm-wire"

static mbedtls_gcm_context s_gcm;
static SemaphoreHandle_t s_lock;
static bool s_ready;
static uint8_t s_tx_prefix[4];
static uint64_t s_tx_counter;
static uint8_t s_rx_prefix[4];
static uint64_t s_rx_last;
static bool s_rx_seen;

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int kvm_crypto_set_key(const char *secret)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (secret == NULL || secret[0] == '\0') {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_ready) {
            mbedtls_gcm_free(&s_gcm);
            mbedtls_gcm_init(&s_gcm);
        }
        s_ready = false;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "端到端加密：关闭");
        return 0;
    }

    uint8_t key[32];
    bool is_hex = strlen(secret) == 64;
    if (is_hex) {
        for (int i = 0; i < 32; i++) {
            int hi = hex_val(secret[i * 2]);
            int lo = hex_val(secret[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                is_hex = false;
                break;
            }
            key[i] = (uint8_t)((hi << 4) | lo);
        }
    }
    if (!is_hex) {
        mbedtls_sha256((const unsigned char *)secret, strlen(secret), key, 0);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ready) {
        mbedtls_gcm_free(&s_gcm);
    }
    mbedtls_gcm_init(&s_gcm);
    int ret = mbedtls_gcm_setkey(&s_gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (ret != 0) {
        ESP_LOGE(TAG, "GCM setkey 失败: -0x%04x", -ret);
        s_ready = false;
    } else {
        memcpy(s_tx_prefix, &(uint32_t){esp_random()}, 4);
        s_tx_counter = 0;
        s_rx_seen = false;
        s_rx_last = 0;
        s_ready = true;
        ESP_LOGI(TAG, "端到端加密：已启用 (AES-256-GCM)");
    }
    xSemaphoreGive(s_lock);
    return s_ready ? 0 : -1;
}

void kvm_crypto_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    mbedtls_gcm_init(&s_gcm);
}

bool kvm_crypto_enabled(void)
{
    return s_ready;
}

int kvm_crypto_encrypt(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    if (!s_ready || in == NULL || out == NULL || cap < MAGIC_LEN + NONCE_LEN + len + TAG_LEN) {
        return -1;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return -1;
    }
    uint8_t nonce[NONCE_LEN];
    memcpy(nonce, s_tx_prefix, 4);
    s_tx_counter++;
    for (int i = 0; i < 8; i++) {
        nonce[4 + i] = (uint8_t)(s_tx_counter >> (56 - i * 8));
    }
    memcpy(out, MAGIC, MAGIC_LEN);
    memcpy(out + MAGIC_LEN, nonce, NONCE_LEN);
    int ret = mbedtls_gcm_crypt_and_tag(&s_gcm, MBEDTLS_GCM_ENCRYPT, len, nonce, NONCE_LEN,
                                        (const unsigned char *)AAD, strlen(AAD),
                                        in, out + MAGIC_LEN + NONCE_LEN, TAG_LEN,
                                        out + MAGIC_LEN + NONCE_LEN + len);
    xSemaphoreGive(s_lock);
    if (ret != 0) {
        ESP_LOGW(TAG, "加密失败: -0x%04x", -ret);
        return -1;
    }
    return (int)(MAGIC_LEN + NONCE_LEN + len + TAG_LEN);
}

int kvm_crypto_decrypt(const uint8_t *in, size_t len, uint8_t *out, size_t cap)
{
    if (!s_ready || in == NULL || out == NULL) {
        return -1;
    }
    if (len < MAGIC_LEN + NONCE_LEN + TAG_LEN || memcmp(in, MAGIC, MAGIC_LEN) != 0) {
        return -1;
    }
    uint64_t counter = 0;
    const uint8_t *nonce = in + MAGIC_LEN;
    for (int i = 0; i < 8; i++) {
        counter = (counter << 8) | nonce[4 + i];
    }
    size_t ct_len = len - MAGIC_LEN - NONCE_LEN - TAG_LEN;
    if (ct_len > cap) {
        return -1;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return -1;
    }
    if (s_rx_seen && memcmp(nonce, s_rx_prefix, 4) == 0 && counter <= s_rx_last) {
        xSemaphoreGive(s_lock);
        return -1; /* 重放 */
    }
    int ret = mbedtls_gcm_auth_decrypt(&s_gcm, ct_len, nonce, NONCE_LEN,
                                       (const unsigned char *)AAD, strlen(AAD),
                                       in + MAGIC_LEN + NONCE_LEN + ct_len, TAG_LEN,
                                       in + MAGIC_LEN + NONCE_LEN, out);
    if (ret == 0) {
        memcpy(s_rx_prefix, nonce, 4);
        s_rx_last = counter;
        s_rx_seen = true;
    }
    xSemaphoreGive(s_lock);
    return ret == 0 ? (int)ct_len : -1;
}

