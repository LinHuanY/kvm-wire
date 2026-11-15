#pragma once

#include <stdbool.h>
#include <stdint.h>

void kvm_input_init(void);

/* BLE 回调里调用：保存"当前按下集合" */
void kvm_input_keyboard(uint8_t mods, const uint16_t *usages, uint8_t count);
void kvm_input_consumer(uint16_t usage);

/* 鼠标位移会累加，按键状态取最新值；dx/dy 支持 16 位（BLE 报告常见 ±2047） */
void kvm_input_mouse(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan);

void kvm_input_release_all(void);
void kvm_input_resync(void);
bool kvm_input_any_pressed(void);

/* 网络任务调用：取出待发送内容 */
bool kvm_input_take_keyboard(uint8_t *mods, uint16_t *usages, uint8_t *count);
bool kvm_input_take_consumer(uint16_t *usage);
bool kvm_input_take_mouse(uint8_t *buttons, int16_t *dx, int16_t *dy, int16_t *wheel, int16_t *pan);
