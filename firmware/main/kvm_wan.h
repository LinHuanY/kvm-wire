#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void kvm_wan_start(void);

/* 中继已连上且已收到 PEER_READY */
bool kvm_wan_connected(void);

/* 把一整个 KVM-Wire 帧通过中继发出去；返回 0 成功，-1 失败 */
int kvm_wan_send(const uint8_t *data, size_t len);

/* 配置变化后调用，让中继任务断开重连 */
void kvm_wan_reload(void);

/* 供 kvm_net 注册：收到中继转来的 KVM 帧时回调 */
typedef void (*kvm_wan_frame_cb_t)(const uint8_t *data, size_t len);
void kvm_wan_set_frame_cb(kvm_wan_frame_cb_t cb);

const char *kvm_wan_status_text(void);
