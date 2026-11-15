#include "kvm_input.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "kvm_proto.h"

#define KEY_RESYNC_INTERVAL_MS 500

static SemaphoreHandle_t s_lock;

static uint8_t s_mods;
static uint16_t s_usages[8];
static uint8_t s_count;
static bool s_kb_dirty;
static int64_t s_kb_last_sent_ms;

static bool s_consumer_pending;
static uint16_t s_consumer_usage;

static int32_t s_dx;
static int32_t s_dy;
static int32_t s_wheel;
static int32_t s_pan;
static uint8_t s_buttons;
static bool s_buttons_dirty;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

void kvm_input_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(s_usages, 0, sizeof(s_usages));
    s_kb_last_sent_ms = now_ms();
}

void kvm_input_keyboard(uint8_t mods, const uint16_t *usages, uint8_t count)
{
    if (count > 8) {
        count = 8;
    }
    lock();
    bool changed = (s_mods != mods) || (s_count != count);
    if (!changed && count) {
        changed = memcmp(s_usages, usages, count * sizeof(uint16_t)) != 0;
    }
    if (changed) {
        s_mods = mods;
        s_count = count;
        if (count) {
            memcpy(s_usages, usages, count * sizeof(uint16_t));
        } else {
            memset(s_usages, 0, sizeof(s_usages));
        }
        s_kb_dirty = true;
    }
    unlock();
}

void kvm_input_consumer(uint16_t usage)
{
    if (usage == 0) {
        return;
    }
    lock();
    s_consumer_usage = usage;
    s_consumer_pending = true;
    unlock();
}

void kvm_input_mouse(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel, int8_t pan)
{
    lock();
    s_dx += dx;
    s_dy += dy;
    s_wheel += wheel;
    s_pan += pan;
    if (s_buttons != buttons) {
        s_buttons = buttons;
        s_buttons_dirty = true;
    }
    unlock();
}

void kvm_input_release_all(void)
{
    lock();
    s_mods = 0;
    s_count = 0;
    memset(s_usages, 0, sizeof(s_usages));
    s_kb_dirty = true;
    s_consumer_pending = false;
    s_buttons = 0;
    s_buttons_dirty = true;
    s_dx = s_dy = s_wheel = s_pan = 0;
    unlock();
}

void kvm_input_resync(void)
{
    lock();
    s_kb_dirty = true;
    s_buttons_dirty = true;
    unlock();
}

bool kvm_input_any_pressed(void)
{
    lock();
    bool pressed = (s_count > 0) || (s_buttons != 0);
    unlock();
    return pressed;
}

bool kvm_input_take_keyboard(uint8_t *mods, uint16_t *usages, uint8_t *count)
{
    bool send = false;
    lock();
    int64_t now = now_ms();
    if (s_kb_dirty || (s_count > 0 && now - s_kb_last_sent_ms >= KEY_RESYNC_INTERVAL_MS)) {
        *mods = s_mods;
        *count = s_count;
        if (s_count) {
            memcpy(usages, s_usages, s_count * sizeof(uint16_t));
        }
        s_kb_dirty = false;
        s_kb_last_sent_ms = now;
        send = true;
    }
    unlock();
    return send;
}

bool kvm_input_take_consumer(uint16_t *usage)
{
    bool send = false;
    lock();
    if (s_consumer_pending) {
        *usage = s_consumer_usage;
        s_consumer_pending = false;
        send = true;
    }
    unlock();
    return send;
}

bool kvm_input_take_mouse(uint8_t *buttons, int16_t *dx, int16_t *dy, int16_t *wheel, int16_t *pan)
{
    bool send = false;
    lock();
    if (s_dx || s_dy || s_wheel || s_pan || s_buttons_dirty) {
        *buttons = s_buttons;
        *dx = kvm_clamp_i16(s_dx);
        *dy = kvm_clamp_i16(s_dy);
        *wheel = kvm_clamp_i16(s_wheel);
        *pan = kvm_clamp_i16(s_pan);
        s_dx = s_dy = s_wheel = s_pan = 0;
        s_buttons_dirty = false;
        send = true;
    }
    unlock();
    return send;
}
