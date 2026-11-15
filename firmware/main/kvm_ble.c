/*
 * BLE HID Host：扫描 BLE 键鼠 -> 连接 -> 解析 HID 报告 -> 交给 kvm_input。
 * 使用 Bluedroid + esp_hid 组件（CONFIG_BT_BLE_ENABLED）。
 */
#include "kvm_ble.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_hid_common.h"
#include "esp_hidh.h"
#include "esp_hidh_gattc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kvm_config.h"
#include "kvm_input.h"
#include "kvm_net.h"
#include "kvm_proto.h"
#include "sdkconfig.h"

static const char *TAG = "kvm_ble";

#ifndef CONFIG_KVM_BLE_AUTO_RECONNECT
#define CONFIG_KVM_BLE_AUTO_RECONNECT 0
#endif

#define HID_SVC_UUID16 0x1812

typedef enum {
    BLE_CMD_SCAN = 1,
    BLE_CMD_CONNECT,
    BLE_CMD_DISCONNECT,
} ble_cmd_t;

typedef struct {
    ble_cmd_t cmd;
    uint8_t bda[6];
    uint8_t addr_type;
    uint32_t duration_ms;
    bool whitelist;
} ble_msg_t;

typedef struct {
    bool used;
    uint8_t bda[6];
    uint8_t addr_type;
    uint8_t usage;
    int8_t battery;
    int8_t rssi;
    bool connected;
    esp_hidh_dev_t *dev;
    char name[33];
    uint8_t retries;
} conn_slot_t;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_scan_param_sem;
static QueueHandle_t s_queue;
static esp_timer_handle_t s_reconnect_timer;
static uint8_t s_reconnect_bda[6];
static uint8_t s_reconnect_addr_type;

static kvm_ble_scan_dev_t s_scan[KVM_BLE_MAX_SCAN];
static int s_scan_count;
static bool s_scanning;

static conn_slot_t s_conns[KVM_BLE_MAX_CONNS];

static void wl_add(const uint8_t bda[6], uint8_t addr_type)
{
    esp_ble_wl_addr_type_t t = (addr_type == 0) ? BLE_WL_ADDR_TYPE_PUBLIC : BLE_WL_ADDR_TYPE_RANDOM;
    esp_ble_gap_update_whitelist(true, (uint8_t *)bda, t);
}

static void wl_sync(void)
{
    for (uint8_t i = 0; i < g_cfg.dev_count; i++) {
        wl_add(g_cfg.devs[i].bda, g_cfg.devs[i].addr_type);
    }
    ESP_LOGI(TAG, "BLE 白名单同步: %u 台已保存设备", g_cfg.dev_count);
}
static volatile bool s_connecting;
static uint8_t s_report_class[256]; /* report_id -> KVM_CLASS_*，用于 usage 解析不到时兜底 */
static int s_input_logs;

static esp_ble_scan_params_t s_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE,
};

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static const char *bda_str(const uint8_t bda[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
    return out;
}

static uint8_t usage_from_appearance(uint16_t appearance)
{
    switch (appearance) {
    case 0x03C1:
        return KVM_CLASS_KEYBOARD;
    case 0x03C2:
        return KVM_CLASS_MOUSE;
    case 0x03C3:
        return KVM_CLASS_KEYBOARD; /* 键鼠一体，先按键盘处理 */
    default:
        return 0;
    }
}

static uint8_t usage_class(esp_hid_usage_t usage)
{
    switch (usage) {
    case ESP_HID_USAGE_KEYBOARD:
        return KVM_CLASS_KEYBOARD;
    case ESP_HID_USAGE_MOUSE:
        return KVM_CLASS_MOUSE;
    case ESP_HID_USAGE_CCONTROL:
        return KVM_CLASS_CONSUMER;
    default:
        return 0;
    }
}

static conn_slot_t *find_conn(const uint8_t bda[6])
{
    for (int i = 0; i < KVM_BLE_MAX_CONNS; i++) {
        if (s_conns[i].used && memcmp(s_conns[i].bda, bda, 6) == 0) {
            return &s_conns[i];
        }
    }
    return NULL;
}

static conn_slot_t *alloc_conn(const uint8_t bda[6])
{
    conn_slot_t *slot = find_conn(bda);
    if (slot) {
        return slot;
    }
    for (int i = 0; i < KVM_BLE_MAX_CONNS; i++) {
        if (!s_conns[i].used) {
            memset(&s_conns[i], 0, sizeof(s_conns[i]));
            s_conns[i].used = true;
            s_conns[i].battery = -1;
            s_conns[i].rssi = -127;
            memcpy(s_conns[i].bda, bda, 6);
            return &s_conns[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* HID 报告解析                                                        */
/* ------------------------------------------------------------------ */

static void log_raw_report(const char *what, const uint8_t *data, uint16_t len)
{
    if (len == 0 || len > 32) {
        return;
    }
    char buf[3 * 32 + 1];
    int pos = 0;
    for (int i = 0; i < len && pos < (int)sizeof(buf) - 4; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X ", data[i]);
    }
    ESP_LOGD(TAG, "%s len=%u data=[%s]", what, len, buf);
}

static void handle_keyboard_report(const uint8_t *data, uint16_t len)
{
    if (len < 7) {
        log_raw_report("keyboard-short", data, len);
        return;
    }
    uint8_t mods = data[0];
    const uint8_t *keys;
    uint16_t keys_len;
    if (len >= 8) {
        keys = data + 2; /* [mods][reserved][k1..k6] */
        keys_len = (uint16_t)(len - 2);
    } else {
        keys = data + 1; /* [mods][k1..k6] */
        keys_len = (uint16_t)(len - 1);
    }
    uint16_t usages[8];
    uint8_t count = 0;
    for (uint16_t i = 0; i < keys_len && count < 6; i++) {
        if (keys[i] != 0 && keys[i] != 0x01) {
            usages[count++] = keys[i];
        }
    }
    kvm_input_keyboard(mods, usages, count);
}

static void handle_consumer_report(const uint8_t *data, uint16_t len)
{
    if (len < 2) {
        log_raw_report("consumer-short", data, len);
        return;
    }
    uint16_t usage = (uint16_t)(data[0] | (data[1] << 8));
    kvm_input_consumer(usage);
}

static void handle_mouse_report(const uint8_t *data, uint16_t len)
{
    if (len >= 6) {
        /* 常见 BLE 鼠标（report mode）：buttons + X16 + Y16 + wheel (+pan) */
        uint8_t buttons = data[0];
        int16_t dx = (int16_t)(data[1] | (data[2] << 8));
        int16_t dy = (int16_t)(data[3] | (data[4] << 8));
        int8_t wheel = (int8_t)data[5];
        int8_t pan = len >= 7 ? (int8_t)data[6] : 0;
        kvm_input_mouse(buttons, dx, dy, wheel, pan);
        return;
    }
    if (len < 3) {
        log_raw_report("mouse-short", data, len);
        return;
    }
    /* boot 协议：buttons + X8 + Y8 + wheel(+pan) */
    uint8_t buttons = data[0];
    int16_t dx = (int8_t)data[1];
    int16_t dy = (int8_t)data[2];
    int8_t wheel = len >= 4 ? (int8_t)data[3] : 0;
    int8_t pan = len >= 5 ? (int8_t)data[4] : 0;
    kvm_input_mouse(buttons, dx, dy, wheel, pan);
}

/* ------------------------------------------------------------------ */
/* esp_hidh 回调                                                       */
/* ------------------------------------------------------------------ */

static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;
    char addr[18];

    switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT: {
        if (param->open.status != ESP_OK) {
            ESP_LOGE(TAG, "设备打开失败 status=%d", param->open.status);
            break;
        }
        const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
        if (bda == NULL) {
            break;
        }
        esp_hid_usage_t usage = esp_hidh_dev_usage_get(param->open.dev);
        uint8_t cls = usage_class(usage);
        const char *name = esp_hidh_dev_name_get(param->open.dev);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        conn_slot_t *slot = alloc_conn(bda);
        if (slot) {
            slot->dev = param->open.dev;
            slot->connected = true;
            slot->usage = cls;
            slot->retries = 0;
            if (name) {
                strlcpy(slot->name, name, sizeof(slot->name));
            }
        }
        xSemaphoreGive(s_lock);

        ESP_LOGI(TAG, "已连接 %s usage=%s(%d) name=%s", bda_str(bda, addr), esp_hid_usage_str(usage), (int)usage,
                 name ? name : "?");
        /* 打印 report 列表，并从 report 推断设备类型（很多键鼠的 device usage 是 GENERIC） */
        uint8_t inferred = 0;
        size_t nrep = 0;
        esp_hid_report_item_t *reports = NULL;
        if (esp_hidh_dev_reports_get(param->open.dev, &nrep, &reports) == ESP_OK && reports) {
            for (size_t i = 0; i < nrep; i++) {
                uint8_t c = usage_class(reports[i].usage);
                if (c && reports[i].report_id < 256) {
                    s_report_class[reports[i].report_id] = c;
                }
                if (reports[i].report_type == ESP_HID_REPORT_TYPE_INPUT && c) {
                    if (c == KVM_CLASS_MOUSE) {
                        inferred = c; /* 鼠标优先，兼容键鼠一体设备 */
                    } else if (!inferred) {
                        inferred = c;
                    }
                }
                ESP_LOGI(TAG, "  report id=%u type=%u usage=%s(%d) len=%u", reports[i].report_id,
                         reports[i].report_type, esp_hid_usage_str(reports[i].usage), (int)reports[i].usage,
                         reports[i].value_len);
            }
        } else {
            ESP_LOGW(TAG, "  无法获取 report 列表");
        }
        if (cls == 0) {
            cls = inferred;
        }
        if (slot) {
            slot->usage = cls;
        }
        if (slot && cls) {
            kvm_config_add_device(bda, slot->addr_type, cls);
        wl_add(bda, slot->addr_type);
        }
        ESP_LOGI(TAG, "设备类型推断: %u", cls);
        size_t nmaps = 0;
        esp_hid_raw_report_map_t *maps = NULL;
        if (esp_hidh_dev_report_maps_get(param->open.dev, &nmaps, &maps) == ESP_OK && maps && maps[0].len) {
            ESP_LOGI(TAG, "  report map len=%u", maps[0].len);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, maps[0].data, maps[0].len > 48 ? 48 : maps[0].len, ESP_LOG_INFO);
        }
        break;
    }

    case ESP_HIDH_CLOSE_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->close.dev);
        if (bda) {
            ESP_LOGW(TAG, "设备断开 %s reason=0x%x", bda_str(bda, addr), param->close.reason);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            conn_slot_t *slot = find_conn(bda);
            if (slot) {
                slot->connected = false;
                slot->dev = NULL;
            }
            xSemaphoreGive(s_lock);
            kvm_input_release_all();
            kvm_net_send_release();
        }
        esp_hidh_dev_free(param->close.dev);

        if (bda && g_cfg.auto_connect) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            conn_slot_t *slot = find_conn(bda);
            bool retry = slot && slot->retries < 5;
            uint8_t attempt = 0;
            if (retry) {
                slot->retries++;
                attempt = slot->retries;
                memcpy(s_reconnect_bda, bda, 6);
                s_reconnect_addr_type = slot->addr_type;
            }
            xSemaphoreGive(s_lock);
            if (retry) {
                ESP_LOGI(TAG, "3 秒后重连 %s (第 %d 次)", bda_str(bda, addr), attempt);
                esp_timer_stop(s_reconnect_timer);
                esp_timer_start_once(s_reconnect_timer, 3 * 1000 * 1000);
            }
        }
        break;
    }

    case ESP_HIDH_BATTERY_EVENT: {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->battery.dev);
        if (bda) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            conn_slot_t *slot = find_conn(bda);
            if (slot) {
                slot->battery = (int8_t)param->battery.level;
            }
            xSemaphoreGive(s_lock);
            kvm_net_request_status();
        }
        break;
    }

    case ESP_HIDH_INPUT_EVENT: {
        esp_hid_usage_t usage = param->input.usage;
        uint8_t cls = usage_class(usage);
        if (cls == 0 && param->input.report_id < 256) {
            cls = s_report_class[param->input.report_id];
        }
        log_raw_report(esp_hid_usage_str(usage), param->input.data, param->input.length);
        if (s_input_logs < 20) {
            s_input_logs++;
            char hex[3 * 24 + 1];
            int pos = 0;
            for (int i = 0; i < param->input.length && i < 24 && pos < (int)sizeof(hex) - 4; i++) {
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", param->input.data[i]);
            }
            ESP_LOGI(TAG, "INPUT %s(%d) id=%u cls=%u len=%u data=[%s]", esp_hid_usage_str(usage), (int)usage,
                     param->input.report_id, cls, param->input.length, hex);
        }
        switch (cls) {
        case KVM_CLASS_KEYBOARD:
            handle_keyboard_report(param->input.data, param->input.length);
            break;
        case KVM_CLASS_MOUSE:
            handle_mouse_report(param->input.data, param->input.length);
            break;
        case KVM_CLASS_CONSUMER:
            handle_consumer_report(param->input.data, param->input.length);
            break;
        default:
            break;
        }
        break;
    }

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* GAP                                                                 */
/* ------------------------------------------------------------------ */

static void add_scan_result(const uint8_t *bda, uint8_t addr_type, int8_t rssi, uint16_t appearance,
                            uint8_t usage, bool hid, const char *name)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    kvm_ble_scan_dev_t *dev = NULL;
    for (int i = 0; i < s_scan_count; i++) {
        if (memcmp(s_scan[i].bda, bda, 6) == 0) {
            dev = &s_scan[i];
            break;
        }
    }
    if (dev == NULL) {
        if (s_scan_count >= KVM_BLE_MAX_SCAN) {
            xSemaphoreGive(s_lock);
            return;
        }
        dev = &s_scan[s_scan_count++];
        memset(dev, 0, sizeof(*dev));
        memcpy(dev->bda, bda, 6);
    }
    dev->addr_type = addr_type;
    dev->rssi = rssi;
    dev->appearance = appearance;
    dev->hid = hid;
    if (usage) {
        dev->usage = usage;
    }
    if (name && name[0]) {
        strlcpy(dev->name, name, sizeof(dev->name));
    }
    xSemaphoreGive(s_lock);
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (s_scan_param_sem) {
            xSemaphoreGive(s_scan_param_sem);
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *p = param;
        if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            uint16_t appearance = 0;
            uint8_t adv_len = 0;
            uint8_t *adv = p->scan_rst.ble_adv;
            uint16_t total_len = (uint16_t)(p->scan_rst.adv_data_len + p->scan_rst.scan_rsp_len);

            uint8_t *d = esp_ble_resolve_adv_data_by_type(adv, total_len, ESP_BLE_AD_TYPE_APPEARANCE, &adv_len);
            if (d && adv_len >= 2) {
                appearance = (uint16_t)(d[0] | (d[1] << 8));
            }

            char name[33] = {0};
            d = esp_ble_resolve_adv_data_by_type(adv, total_len, ESP_BLE_AD_TYPE_NAME_CMPL, &adv_len);
            if (d == NULL) {
                d = esp_ble_resolve_adv_data_by_type(adv, total_len, ESP_BLE_AD_TYPE_NAME_SHORT, &adv_len);
            }
            if (d && adv_len) {
                uint8_t n = adv_len < sizeof(name) - 1 ? adv_len : (uint8_t)(sizeof(name) - 1);
                memcpy(name, d, n);
                name[n] = 0;
            }

            bool hid = false;
            d = esp_ble_resolve_adv_data_by_type(adv, total_len, ESP_BLE_AD_TYPE_16SRV_CMPL, &adv_len);
            if (d && adv_len >= 2) {
                for (uint8_t i = 0; i + 1 < adv_len; i += 2) {
                    if ((uint16_t)(d[i] | (d[i + 1] << 8)) == HID_SVC_UUID16) {
                        hid = true;
                        break;
                    }
                }
            }
            uint8_t usage = usage_from_appearance(appearance);
            if (usage) {
                hid = true;
            }
            add_scan_result(p->scan_rst.bda, p->scan_rst.ble_addr_type, p->scan_rst.rssi, appearance, usage,
                            hid, name);
        } else if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_scanning = false;
            int count = s_scan_count;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "BLE 扫描完成，共 %d 个设备", count);
        }
        break;
    }

    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
        ESP_LOGI(TAG, "配对%s addr=%02X:%02X:%02X:%02X:%02X:%02X",
                 param->ble_security.auth_cmpl.success ? "成功" : "失败",
                 param->ble_security.auth_cmpl.bd_addr[0], param->ble_security.auth_cmpl.bd_addr[1],
                 param->ble_security.auth_cmpl.bd_addr[2], param->ble_security.auth_cmpl.bd_addr[3],
                 param->ble_security.auth_cmpl.bd_addr[4], param->ble_security.auth_cmpl.bd_addr[5]);
        if (!param->ble_security.auth_cmpl.success) {
            ESP_LOGW(TAG, "配对失败 reason=0x%x（可尝试关闭 KVM_BLE_BOND）",
                     param->ble_security.auth_cmpl.fail_reason);
        }
        break;
    }

    case ESP_GAP_BLE_SEC_REQ_EVT:
        ESP_LOGI(TAG, "收到安全请求，接受配对");
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        ESP_LOGI(TAG, "passkey=%06" PRIu32 "（设备若要求输入请输入）",
                 param->ble_security.key_notif.passkey);
        break;

    case ESP_GAP_BLE_NC_REQ_EVT:
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 重连定时器                                                          */
/* ------------------------------------------------------------------ */

static void reconnect_cb(void *arg)
{
    (void)arg;
    ble_msg_t msg = {.cmd = BLE_CMD_CONNECT, .addr_type = s_reconnect_addr_type};
    memcpy(msg.bda, s_reconnect_bda, 6);
    if (s_queue) {
        xQueueSend(s_queue, &msg, 0);
    }
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

static void ble_set_security(void)
{
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint32_t passkey = 123456;

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof(passkey));
}

static void ble_worker(void *arg);

/* ------------------------------------------------------------------ */
/* 按名字自动重连                                                      */
/* ------------------------------------------------------------------ */
static int64_t s_auto_last_scan_ms;
static int s_auto_tried_idx;

typedef struct {
    uint8_t bda[6];
    int64_t t_ms;
} auto_tried_t;
static auto_tried_t s_auto_tried[8];

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0 || hay == NULL) {
        return false;
    }
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static bool auto_name_match(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    char buf[96];
    strlcpy(buf, CONFIG_KVM_BLE_AUTO_NAMES, sizeof(buf));
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') {
            tok++;
        }
        if (contains_ci(name, tok)) {
            return true;
        }
    }
    return false;
}

static bool auto_already_connected(const uint8_t bda[6])
{
    kvm_ble_conn_t conns[KVM_BLE_MAX_CONNS];
    int n = kvm_ble_conn_get(conns, KVM_BLE_MAX_CONNS);
    for (int i = 0; i < n; i++) {
        if (memcmp(conns[i].bda, bda, 6) == 0) {
            return true;
        }
    }
    return false;
}

static bool auto_tried_recent(const uint8_t bda[6], int64_t now_ms)
{
    for (int i = 0; i < 8; i++) {
        if (memcmp(s_auto_tried[i].bda, bda, 6) == 0 && now_ms - s_auto_tried[i].t_ms < 120000) {
            return true;
        }
    }
    return false;
}

static void auto_mark_tried(const uint8_t bda[6], int64_t now_ms)
{
    auto_tried_t *t = &s_auto_tried[s_auto_tried_idx];
    s_auto_tried_idx = (s_auto_tried_idx + 1) % 8;
    memcpy(t->bda, bda, 6);
    t->t_ms = now_ms;
}

static void auto_connect_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(12000));
    while (1) {
        if (!CONFIG_KVM_BLE_AUTO_RECONNECT || s_connecting || kvm_ble_conn_count() >= 2) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        int64_t now = esp_timer_get_time() / 1000;
        if (now - s_auto_last_scan_ms < 15000) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        s_auto_last_scan_ms = now;
        static int empty_wl;
        bool use_wl = empty_wl < 3;
        if (kvm_ble_scan_start_ex(4000, use_wl) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(5200));
        kvm_ble_scan_dev_t devs[KVM_BLE_MAX_SCAN];
        int n = kvm_ble_scan_get(devs, KVM_BLE_MAX_SCAN);
        if (use_wl) {
            empty_wl = (n == 0) ? empty_wl + 1 : 0;
        }
        for (int i = 0; i < n; i++) {
            bool saved = kvm_config_has_device(devs[i].bda);
            if (devs[i].rssi < -90 || devs[i].rssi == 0) {
                continue;
            }
            if (!(saved || auto_name_match(devs[i].name)) || auto_already_connected(devs[i].bda) ||
                auto_tried_recent(devs[i].bda, now)) {
                continue;
            }
            char addr[18];
            ESP_LOGI(TAG, "自动重连%s %s (%s)", saved ? "(已保存)" : "(按名字)", devs[i].name,
                     bda_str(devs[i].bda, addr));
            auto_mark_tried(devs[i].bda, now);
            kvm_ble_connect(devs[i].bda, devs[i].addr_type);
            vTaskDelay(pdMS_TO_TICKS(7000));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void kvm_ble_start(void)
{
    /* 排查 HID 通知订阅问题：打开 esp_hid 的调试日志 */
    esp_log_level_set("BLE_HIDH", ESP_LOG_DEBUG);
    esp_log_level_set("BT_APPL", ESP_LOG_INFO);

    s_lock = xSemaphoreCreateMutex();
    s_scan_param_sem = xSemaphoreCreateBinary();
    s_queue = xQueueCreate(8, sizeof(ble_msg_t));
    memset(s_report_class, 0, sizeof(s_report_class));
    s_input_logs = 0;

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_cb,
        .name = "ble_reconnect",
    };
    esp_timer_create(&timer_args, &s_reconnect_timer);

    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_set_device_name("KVM-Bridge"));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    wl_sync();

    ble_set_security();

    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));

    esp_hidh_config_t hidh_cfg = {
        .callback = hidh_callback,
        .event_stack_size = 6144,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(esp_hidh_init(&hidh_cfg));

    xTaskCreate(ble_worker, "ble_worker", 10240, NULL, 5, NULL);
    xTaskCreate(auto_connect_task, "ble_auto", 6144, NULL, 3, NULL);

    ESP_LOGI(TAG, "BLE HID Host 已初始化");

    /* 键鼠休眠断开后，由 ble_auto 任务按名字定期扫描并重连；
     * 不直接用 NVS 里的旧地址，避免 RPA 失效导致 esp_hidh_dev_open 长时间阻塞。 */
    ESP_LOGI(TAG, "就绪：键鼠可网页手动连接，也会按名字自动重连");
}

/* ------------------------------------------------------------------ */
/* 工作线程                                                            */
/* ------------------------------------------------------------------ */

static void ble_worker(void *arg)
{
    (void)arg;
    ble_msg_t msg;
    char addr[18];

    while (1) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (msg.cmd) {
        case BLE_CMD_SCAN: {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_scan_count = 0;
            s_scanning = true;
            xSemaphoreGive(s_lock);

            esp_ble_gap_stop_scanning();
            s_scan_params.scan_filter_policy = msg.whitelist ? BLE_SCAN_FILTER_ALLOW_ONLY_WLST
                                                             : BLE_SCAN_FILTER_ALLOW_ALL;
            esp_ble_gap_set_scan_params(&s_scan_params);
            xSemaphoreTake(s_scan_param_sem, pdMS_TO_TICKS(2000));
            uint32_t sec = (msg.duration_ms + 999) / 1000;
            if (sec == 0) {
                sec = 5;
            }
            ESP_LOGI(TAG, "开始 BLE 扫描 %u 秒", (unsigned)sec);
            esp_err_t err = esp_ble_gap_start_scanning(sec);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "启动扫描失败: %s", esp_err_to_name(err));
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_scanning = false;
                xSemaphoreGive(s_lock);
            }
            break;
        }

        case BLE_CMD_CONNECT: {
            esp_ble_gap_stop_scanning();
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_scanning = false;
            conn_slot_t *slot = find_conn(msg.bda);
            bool already = slot && slot->connected;
            if (already == false) {
                /* 先建槽位，保证 OPEN 回调拿到正确的 addr_type */
                slot = alloc_conn(msg.bda);
                if (slot) {
                    slot->addr_type = msg.addr_type;
                }
            }
            xSemaphoreGive(s_lock);
            if (already) {
                ESP_LOGW(TAG, "%s 已连接", bda_str(msg.bda, addr));
                break;
            }
            ESP_LOGI(TAG, "正在连接 %s ...", bda_str(msg.bda, addr));
            s_connecting = true;
            esp_hidh_dev_t *dev = esp_hidh_dev_open((uint8_t *)msg.bda, ESP_HID_TRANSPORT_BLE, msg.addr_type);
            s_connecting = false;
            if (dev == NULL) {
                ESP_LOGE(TAG, "连接 %s 失败", bda_str(msg.bda, addr));
                break;
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            slot = alloc_conn(msg.bda);
            if (slot) {
                slot->addr_type = msg.addr_type;
                slot->connected = true;
                slot->dev = dev;
            }
            xSemaphoreGive(s_lock);

#if CONFIG_KVM_BLE_BOND
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_ble_set_encryption((uint8_t *)msg.bda, ESP_BLE_SEC_ENCRYPT_NO_MITM);
#endif
            ESP_LOGI(TAG, "连接完成 %s", bda_str(msg.bda, addr));
            /* 键鼠对延迟敏感：固定 7.5ms 连接间隔，避免默认 30~50ms */
            esp_ble_gap_set_prefer_conn_params((uint8_t *)msg.bda, 6, 6, 0, 400);
            break;
        }

        case BLE_CMD_DISCONNECT: {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            conn_slot_t *slot = find_conn(msg.bda);
            esp_hidh_dev_t *dev = slot ? slot->dev : NULL;
            if (slot) {
                slot->connected = false;
                slot->dev = NULL;
                slot->retries = 255; /* 用户主动断开，不再自动重连 */
            }
            xSemaphoreGive(s_lock);
            if (dev) {
                esp_hidh_dev_close(dev);
            } else {
                esp_ble_gap_disconnect(msg.bda);
            }
            kvm_config_remove_device(msg.bda);
            ESP_LOGI(TAG, "断开 %s", bda_str(msg.bda, addr));
            break;
        }

        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t kvm_ble_scan_start_ex(uint32_t duration_ms, bool whitelist)
{
    if (!s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (kvm_ble_scanning()) {
        return ESP_ERR_INVALID_STATE;
    }
    ble_msg_t msg = {.cmd = BLE_CMD_SCAN, .duration_ms = duration_ms ? duration_ms : 8000};
    if (xQueueSend(s_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t kvm_ble_scan_start(uint32_t duration_ms)
{
    return kvm_ble_scan_start_ex(duration_ms, false);
}

bool kvm_ble_scanning(void)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_scanning;
    xSemaphoreGive(s_lock);
    return v;
}

int kvm_ble_scan_get(kvm_ble_scan_dev_t *out, int max)
{
    if (!s_lock || out == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_scan_count < max ? s_scan_count : max;
    memcpy(out, s_scan, n * sizeof(kvm_ble_scan_dev_t));
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t kvm_ble_connect(const uint8_t bda[6], uint8_t addr_type)
{
    if (!s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    ble_msg_t msg = {.cmd = BLE_CMD_CONNECT, .addr_type = addr_type};
    memcpy(msg.bda, bda, 6);
    if (xQueueSend(s_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t kvm_ble_disconnect(const uint8_t bda[6])
{
    if (!s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    ble_msg_t msg = {.cmd = BLE_CMD_DISCONNECT};
    memcpy(msg.bda, bda, 6);
    if (xQueueSend(s_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

int kvm_ble_conn_get(kvm_ble_conn_t *out, int max)
{
    if (!s_lock || out == NULL) {
        return 0;
    }
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < KVM_BLE_MAX_CONNS && n < max; i++) {
        if (!s_conns[i].used) {
            continue;
        }
        out[n].addr_type = s_conns[i].addr_type;
        out[n].usage = s_conns[i].usage;
        out[n].battery = s_conns[i].battery;
        out[n].rssi = s_conns[i].rssi;
        out[n].connected = s_conns[i].connected;
        memcpy(out[n].bda, s_conns[i].bda, 6);
        strlcpy(out[n].name, s_conns[i].name, sizeof(out[n].name));
        n++;
    }
    xSemaphoreGive(s_lock);
    return n;
}

int kvm_ble_conn_count(void)
{
    int count = 0;
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < KVM_BLE_MAX_CONNS; i++) {
        if (s_conns[i].used && s_conns[i].connected) {
            count++;
        }
    }
    xSemaphoreGive(s_lock);
    return count;
}

bool kvm_ble_any_connected(void)
{
    return kvm_ble_conn_count() > 0;
}


