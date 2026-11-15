#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 端到端加密：AES-256-GCM，密钥由网页/配置提供。
 * 帧格式：magic "KVE1" + nonce(12) + ciphertext + tag(16)
 */
void kvm_crypto_init(void);
bool kvm_crypto_enabled(void);
int kvm_crypto_set_key(const char *secret);

/* 返回密文长度，<0 失败 */
int kvm_crypto_encrypt(const uint8_t *in, size_t len, uint8_t *out, size_t cap);
/* 返回明文长度，<0 失败/认证失败/重放 */
int kvm_crypto_decrypt(const uint8_t *in, size_t len, uint8_t *out, size_t cap);
