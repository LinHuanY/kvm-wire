#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void kvm_net_start(void);
void kvm_net_on_link_up(void);
void kvm_net_on_link_down(void);

/* 广域网中继收到一个完整 KVM 帧时调用 */
void kvm_net_handle_frame(const uint8_t *data, size_t len);

void kvm_net_send_release(void);
void kvm_net_request_status(void);
void kvm_net_apply_config(void);

bool kvm_net_host_online(void);
int kvm_net_host_idle_ms(void);
uint32_t kvm_net_tx_count(void);
uint32_t kvm_net_rx_count(void);
uint32_t kvm_net_lost_count(void);
