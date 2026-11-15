/*
 * KVM-Wire v1 协议常量与编解码（与 host/kvmproto.py 保持一致）
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KVM_MAGIC0 0x4B /* 'K' */
#define KVM_MAGIC1 0x56 /* 'V' */
#define KVM_VERSION 1
#define KVM_HDR_LEN 21
#define KVM_MAX_PAYLOAD 255

#define KVM_MSG_HELLO 0x01
#define KVM_MSG_KEY 0x02
#define KVM_MSG_MOUSE 0x03
#define KVM_MSG_PING 0x04
#define KVM_MSG_PONG 0x05
#define KVM_MSG_CMD 0x06
#define KVM_MSG_STATUS 0x07
#define KVM_MSG_RELEASE 0x08
#define KVM_MSG_DISCOVER 0x09
#define KVM_MSG_KEEPALIVE 0x0A

/* 0x10~0x1F 预留给广域网中继/会话控制（见 docs/wan.md） */

#define KVM_PAGE_KEYBOARD 0x07
#define KVM_PAGE_CONSUMER 0x0C

#define KVM_CMD_RESYNC 1
#define KVM_CMD_RELEASE_ALL 2
#define KVM_CMD_SCAN 3

#define KVM_MOUSE_ABS 0x01

#define KVM_I16_MAX 32767
#define KVM_I16_MIN (-32768)

/* 设备分类（STATUS/网页里用） */
#define KVM_CLASS_KEYBOARD 1
#define KVM_CLASS_MOUSE 2
#define KVM_CLASS_CONSUMER 3

typedef struct {
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint16_t seq;
    uint64_t token;
    uint32_t dev_ms;
    uint8_t plen;
    const uint8_t *payload;
} kvm_frame_t;

static inline void kvm_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void kvm_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline void kvm_put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    }
}

static inline uint16_t kvm_get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t kvm_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t kvm_get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t)p[i]) << (8 * i);
    }
    return v;
}

static inline int16_t kvm_clamp_i16(int32_t v)
{
    if (v > KVM_I16_MAX) {
        return KVM_I16_MAX;
    }
    if (v < KVM_I16_MIN) {
        return KVM_I16_MIN;
    }
    return (int16_t)v;
}

/* 返回写出的总长度（头 + 负载），0 表示失败 */
static inline size_t kvm_frame_encode(uint8_t *out, size_t cap, uint8_t type, uint16_t seq, uint64_t token,
                                      uint32_t dev_ms, const uint8_t *payload, uint8_t plen)
{
    if (cap < KVM_HDR_LEN + plen) {
        return 0;
    }
    out[0] = KVM_MAGIC0;
    out[1] = KVM_MAGIC1;
    out[2] = KVM_VERSION;
    out[3] = type;
    kvm_put_u16(out + 4, seq);
    out[6] = 0; /* flags */
    out[7] = 0; /* reserved */
    kvm_put_u64(out + 8, token);
    kvm_put_u32(out + 16, dev_ms);
    out[20] = plen;
    if (plen && payload) {
        memcpy(out + KVM_HDR_LEN, payload, plen);
    }
    return KVM_HDR_LEN + plen;
}

static inline bool kvm_frame_decode(const uint8_t *data, size_t len, kvm_frame_t *out)
{
    if (len < KVM_HDR_LEN || data[0] != KVM_MAGIC0 || data[1] != KVM_MAGIC1) {
        return false;
    }
    if (data[2] != KVM_VERSION) {
        return false;
    }
    uint8_t plen = data[20];
    if (len != (size_t)KVM_HDR_LEN + plen) {
        return false;
    }
    out->version = data[2];
    out->type = data[3];
    out->seq = kvm_get_u16(data + 4);
    out->flags = data[6];
    out->token = kvm_get_u64(data + 8);
    out->dev_ms = kvm_get_u32(data + 16);
    out->plen = plen;
    out->payload = data + KVM_HDR_LEN;
    return true;
}

/* 组 KEY 负载：page + mods + count + usages */
static inline uint8_t kvm_key_payload(uint8_t *buf, uint8_t page, uint8_t mods, const uint16_t *usages, uint8_t count)
{
    if (count > 12) {
        count = 12;
    }
    buf[0] = page;
    buf[1] = mods;
    buf[2] = count;
    for (uint8_t i = 0; i < count; i++) {
        kvm_put_u16(buf + 3 + i * 2, usages[i]);
    }
    return (uint8_t)(3 + count * 2);
}

/* 组 MOUSE 负载 */
static inline uint8_t kvm_mouse_payload(uint8_t *buf, uint8_t buttons, bool absolute, int16_t dx, int16_t dy,
                                        int16_t wheel, int16_t pan)
{
    buf[0] = buttons;
    buf[1] = absolute ? KVM_MOUSE_ABS : 0;
    kvm_put_u16(buf + 2, (uint16_t)dx);
    kvm_put_u16(buf + 4, (uint16_t)dy);
    kvm_put_u16(buf + 6, (uint16_t)wheel);
    kvm_put_u16(buf + 8, (uint16_t)pan);
    return 10;
}
