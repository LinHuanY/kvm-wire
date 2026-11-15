"""端到端加密：设备与 PC 共享 32 字节密钥，AES-256-GCM。

中继只看到密文；即使中继被攻破，也无法读取或伪造键鼠数据。

密钥格式：
- 64 位十六进制（32 字节），或
- 任意口令（内部 SHA-256 派生）
"""

from __future__ import annotations

import hashlib
import os
import struct

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

MAGIC = b"KVE1"
AAD = b"kvm-wire"
NONCE_LEN = 12
TAG_LEN = 16


def derive_key(secret: str) -> bytes:
    secret = secret.strip()
    if len(secret) == 64:
        try:
            return bytes.fromhex(secret)
        except ValueError:
            pass
    return hashlib.sha256(secret.encode("utf-8")).digest()


class E2ECodec:
    def __init__(self, secret: str) -> None:
        self._aead = AESGCM(derive_key(secret))
        self._tx_prefix = os.urandom(4)
        self._tx_counter = 0
        self._rx_prefix: bytes | None = None
        self._rx_last = -1

    def encrypt(self, plaintext: bytes) -> bytes:
        self._tx_counter = (self._tx_counter + 1) & 0xFFFFFFFFFFFFFFFF
        nonce = self._tx_prefix + struct.pack(">Q", self._tx_counter)
        ciphertext = self._aead.encrypt(nonce, plaintext, AAD)
        return MAGIC + nonce + ciphertext

    def decrypt(self, data: bytes) -> bytes | None:
        if len(data) < len(MAGIC) + NONCE_LEN + TAG_LEN or not data.startswith(MAGIC):
            return None
        nonce = data[4:4 + NONCE_LEN]
        prefix = nonce[:4]
        counter = struct.unpack(">Q", nonce[4:])[0]
        if prefix == self._rx_prefix and counter <= self._rx_last:
            return None  # 重放
        try:
            plaintext = self._aead.decrypt(nonce, data[4 + NONCE_LEN:], AAD)
        except Exception:  # noqa: BLE001 - 认证失败
            return None
        self._rx_prefix = prefix
        self._rx_last = counter
        return plaintext
