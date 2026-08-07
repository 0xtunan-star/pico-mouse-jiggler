/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 修改：复合 HID 鼠标 + 键盘，适配 Mac
 * - 鼠标：智能贝塞尔曲线移动 + 随机点击
 * - 键盘：Cmd+S、Cmd+Tab 闪切、Spotlight 误触取消、打字纠错
 * - LED 状态：琥珀启动，蓝 USB 准备，绿主机就绪，紫活动，黄挂起，红错误
 * - BOOTSEL 短按切换启用/禁用，长按无额外功能（保留）
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "ws2812.pio.h"

// ========================================================================
// 硬件配置
// ========================================================================
#define WS2812_PIN         16
#define NUM_PIXELS         1
#define IS_RGBW            false

// ========================================================================
// HID 报告 ID
// ========================================================================
#define REPORT_ID_MOUSE    1
#define REPORT_ID_KEYBOARD 2

// ========================================================================
// WS2812 颜色常量 (RGB 格式)
// ========================================================================
#define COLOR_RED          0xff0000
#define COLOR_GREEN        0x00ff00
#define COLOR_BLUE         0x0000ff
#define COLOR_WHITE        0xffffff
#define COLOR_OFF          0x000000
#define COLOR_AMBER        0xff8000
#define COLOR_YELLOW       0xffff00
#define COLOR_PURPLE       0xff00ff
#define COLOR_RED_DIM      0x200000

// ========================================================================
// 全局变量
// ========================================================================
PIO ws2812_pio;
uint ws2812_sm;
uint ws2812_offset;

static bool usb_host_mounted = false;
static bool usb_suspended    = false;
static bool random_movement_enabled = false;

// 用于 LED 状态闪灯
static bool ws_activity_flash = false;
static uint32_t ws_activity_timestamp = 0;
static uint32_t ws_activity_duration_ms = 0;

// BOOTSEL 按键防抖
static bool bootsel_last_level = false;
static uint32_t bootsel_last_toggle_ms = 0;

// 鼠标任务状态
static uint32_t last_movement_time = 0;
static uint32_t next_movement_interval = 0;   // 下次鼠标动作间隔 (ms)

// 键盘任务状态
static uint32_t next_keyboard_interval = 0;   // 下次键盘动作的绝对时间
static bool keyboard_action_in_progress = false;

// ========================================================================
// 辅助函数
// ========================================================================
static int rand_range(int min, int max) {
    return min + rand() % (max - min + 1);
}

// ========================================================================
// WS2812 LED 驱动
// ========================================================================
static inline void put_pixel(uint32_t pixel_grb) {
    pio_sm_put_blocking(ws2812_pio, ws2812_sm, pixel_grb << 8u);
}

static inline uint32_t rgb_to_grb(uint32_t rgb_color) {
    uint8_t r = (rgb_color >> 16) & 0xff;
    uint8_t g = (rgb_color >> 8) & 0xff;
    uint8_t b = rgb_color & 0xff;
    return (g << 16) | (r << 8) | b;
}

static inline uint32_t dim_color(uint32_t rgb_color, uint8_t shift_bits) {
    uint8_t r = ((rgb_color >> 16) & 0xff) >> shift_bits;
    uint8_t g = ((rgb_color >> 8) & 0xff) >> shift_bits;
    uint8_t b = (rgb_color & 0xff) >> shift_bits;
    return (r << 16) | (g << 8) | b;
}

void set_led_color(uint32_t rgb_color) {
    put_pixel(rgb_to_grb(rgb_color));
}

// ========================================================================
// LED 状态机
// ========================================================================
typedef enum {
    WS_LOG_BOOTING,
    WS_LOG_USB_READY,
    WS_LOG_HOST_READY,
    WS_LOG_ACTIVITY,
    WS_LOG_SUSPENDED,
    WS_LOG_ERROR
} ws_log_state_t;

static void ws2812_log_state(ws_log_state_t state) {
    switch (state) {
        case WS_LOG_BOOTING:   set_led_color(COLOR_AMBER); break;
        case WS_LOG_USB_READY: set_led_color(COLOR_BLUE);  break;
        case WS_LOG_HOST_READY:set_led_color(COLOR_GREEN); break;
        case WS_LOG_ACTIVITY:  set_led_color(COLOR_PURPLE);break;
        case WS_LOG_SUSPENDED: set_led_color(COLOR_YELLOW);break;
        case WS_LOG_ERROR:
        default:               set_led_color(COLOR_RED);   break;
    }
}

static void ws2812_restore_status_color(void) {
    if (usb_suspended) {
        ws2812_log_state(WS_LOG_SUSPENDED);
        return;
    }
    if (!usb_host_mounted) {
        ws2812_log_state(WS_LOG_USB_READY);
        return;
    }
    if (random_movement_enabled) {
        set_led_color(dim_color(COLOR_GREEN, 1)); // 亮绿
    } else {
        set_led_color(dim_color(COLOR_RED, 1));   // 暗红
    }
}

static void ws2812_flash_state(ws_log_state_t state, uint32_t duration_ms) {
    ws2812_log_state(state);
    ws_activity_flash = true;
    ws_activity_timestamp = board_millis();
    ws_activity_duration_ms = duration_ms;
}

static void ws2812_status_task(void) {
    if (!ws_activity_flash) return;
    if (board_millis() - ws_activity_timestamp >= ws_activity_duration_ms) {
        ws_activity_flash = false;
        ws2812_restore_status_color();
    }
}

bool ws2812_init() {
    printf("Initializing WS2812 on GPIO %d...\n", WS2812_PIN);
    bool success = pio_claim_free_sm_and_add_program_for_gpio_range(
        &ws2812_program, &ws2812_pio, &ws2812_sm, &ws2812_offset,
        WS2812_PIN, 1, true);
    if (!success) {
        printf("WS2812 init failed\n");
        return false;
    }
    ws2812_program_init(ws2812_pio, ws2812_sm, ws2812_offset,
                        WS2812_PIN, 800000, IS_RGBW);
    return true;
}

// ========================================================================
// USB HID 报告发送函数
// ========================================================================
static void send_mouse_report(uint8_t buttons, int8_t dx, int8_t dy) {
    tud_hid_report(REPORT_ID_MOUSE, (uint8_t[]){ buttons, dx, dy, 0, 0 }, 5);
}

static void send_keyboard_report(uint8_t modifier, uint8_t key1, uint8_t key2,
                                 uint8_t key3, uint8_t key4, uint8_t key5, uint8_t key6) {
    uint8_t report[8] = { modifier, 0x00, key1, key2, key3, key4, key5, key6 };
    tud_hid_report(REPORT_ID_KEYBOARD, report, 8);
}

// ========================================================================
// 鼠标智能移动任务（保留原有完整逻辑，仅微调）
// ========================================================================
static void hid_task(void) {
    if (!tud_hid_ready()) return;
    if (!random_movement_enabled) return;

    uint32_t now = board_millis();

    // 动态节奏状态机
    typedef enum {
        STATE_IDLE,
        STATE_MOVING,
        STATE_PAUSE,
        STATE_CLICK
    } state_t;
    static state_t state = STATE_IDLE;
    static uint32_t state_until = 0;
    static int step = 0, total_steps = 0;
    static float total_x = 0, total_y = 0;
    static float cur_x = 0, cur_y = 0;
    static uint32_t next_tick = 0;

    if (now < next_tick) return;

    int8_t dx = 0, dy = 0;

    switch (state) {
        case STATE_IDLE:
            if (now > state_until) {
                total_x = rand_range(-200, 200);
                total_y = rand_range(-200, 200);
                if (rand() % 5 == 0) { total_x *= 2; total_y *= 2; }
                float dist = sqrtf(total_x * total_x + total_y * total_y);
                total_steps = (dist > 150) ? rand_range(20, 40) : rand_range(40, 100);
                cur_x = 0; cur_y = 0;
                step = 0;
                state = STATE_MOVING;
                ws2812_log_state(WS_LOG_ACTIVITY);
            }
            break;

        case STATE_MOVING: {
            float t = (float)step / total_steps;
            float ease;
            if (t < 0.3) ease = 3 * t * t;
            else if (t < 0.7) ease = 0.5 + (t - 0.3);
            else ease = 1 - (1 - t) * (1 - t);

            float target_x = total_x * ease;
            float target_y = total_y * ease;
            dx = (int)(target_x - cur_x);
            dy = (int)(target_y - cur_y);
            cur_x = target_x;
            cur_y = target_y;

            dx += rand_range(-1, 1);
            dy += rand_range(-1, 1);

            if (dx != 0 || dy != 0) {
                send_mouse_report(0, dx, dy);
            }
            step++;

            if (step >= total_steps) {
                if (rand() % 5 == 0) {
                    state = STATE_CLICK;
                } else {
                    state = STATE_PAUSE;
                    state_until = now + rand_range(500, 3000);
                }
            }

            // 动态速度
            if (t < 0.2)      next_tick = now + rand_range(2, 6);
            else if (t < 0.8) next_tick = now + rand_range(1, 4);
            else              next_tick = now + rand_range(5, 15);
            break;
        }

        case STATE_CLICK:
            send_mouse_report(0x01, 0, 0); // 左键按下
            sleep_ms(rand_range(50, 120));
            send_mouse_report(0x00, 0, 0); // 释放
            state = STATE_PAUSE;
            state_until = now + rand_range(800, 4000);
            next_tick = now + 50;
            break;

        case STATE_PAUSE:
            if (now > state_until) {
                state = STATE_IDLE;
                state_until = now + rand_range(2000, 8000);
                ws2812_restore_status_color();
            }
            next_tick = now + rand_range(10, 30);
            break;
    }
}

// ========================================================================
// Mac 键盘动作生成
// ========================================================================
static bool generate_mac_keyboard_action(void) {
    if (!random_movement_enabled) return false;
    if (keyboard_action_in_progress) return false;

    // 模拟手离开鼠标延迟
    sleep_ms(rand_range(150, 400));

    int action = rand() % 100;
    uint8_t modifier = 0;
    uint8_t key = 0;

    if (action < 50) {
        // ---- Cmd+S (保存) ----
        modifier = KEYBOARD_MODIFIER_LEFTGUI;   // 0x08
        key = HID_KEY_S;
        send_keyboard_report(modifier, key, 0,0,0,0,0);
        sleep_ms(rand_range(30, 60));
        send_keyboard_report(0, 0,0,0,0,0,0);
        printf("Mac: Cmd+S\n");
    }
    else if (action < 70) {
        // ---- Cmd+Tab 闪切 ----
        modifier = KEYBOARD_MODIFIER_LEFTGUI;
        key = HID_KEY_TAB;
        send_keyboard_report(modifier, key, 0,0,0,0,0);
        sleep_ms(rand_range(60, 120));
        modifier = KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_LEFTSHIFT;
        send_keyboard_report(modifier, key, 0,0,0,0,0);
        sleep_ms(rand_range(40, 80));
        send_keyboard_report(0, 0,0,0,0,0,0);
        printf("Mac: Cmd+Tab flick\n");
    }
    else if (action < 85) {
        // ---- Spotlight 误触取消 ----
        modifier = KEYBOARD_MODIFIER_LEFTGUI;
        key = HID_KEY_SPACE;
        send_keyboard_report(modifier, key, 0,0,0,0,0);
        sleep_ms(rand_range(150, 350));
        send_keyboard_report(0, 0,0,0,0,0,0);
        sleep_ms(rand_range(100, 200));
        send_keyboard_report(0, HID_KEY_ESCAPE, 0,0,0,0,0);
        sleep_ms(rand_range(30, 60));
        send_keyboard_report(0, 0,0,0,0,0,0);
        printf("Mac: Spotlight escape\n");
    }
    else {
        // ---- 打字纠错（teh -> the） ----
        // 输入 't'
        send_keyboard_report(0, HID_KEY_T, 0,0,0,0,0);
        sleep_ms(rand_range(60, 120));
        send_keyboard_report(0, 0,0,0,0,0,0);
        sleep_ms(rand_range(50, 100));
        // 'e'
        send_keyboard_report(0, HID_KEY_E, 0,0,0,0,0);
        sleep_ms(rand_range(60, 120));
        send_keyboard_report(0, 0,0,0,0,0,0);
        sleep_ms(rand_range(50, 100));
        // 'h'
        send_keyboard_report(0, HID_KEY_H, 0,0,0,0,0);
        sleep_ms(rand_range(60, 120));
        send_keyboard_report(0, 0,0,0,0,0,0);
        // 退格删除
        sleep_ms(rand_range(100, 200));
        send_keyboard_report(0, HID_KEY_BACKSPACE, 0,0,0,0,0);
        sleep_ms(rand_range(30, 60));
        send_keyboard_report(0, 0,0,0,0,0,0);
        // 输入 "the "
        send_keyboard_report(0, HID_KEY_T, 0,0,0,0,0);
        sleep_ms(rand_range(60, 120));
        send_keyboard_report(0, 0,0,0,0,0,0);
        sleep_ms(rand_range(50, 100));
        send_keyboard_report(0, HID_KEY_H, 0,0,0,0,0);
        sleep_ms(rand_range(60, 120));
        send_keyboard_report(0, 0,0,0,0,0,0);
        sleep_ms(rand_range(50, 100));
        send_keyboard_report(0, HID_KEY_E, 0,0,0,0,0);
        sleep_ms(rand_range(60, 120));
        send_keyboard_report(0, 0,0,0,0,0,0);
        sleep_ms(rand_range(50, 100));
        send_keyboard_report(0, HID_KEY_SPACE, 0,0,0,0,0);
        sleep_ms(rand_range(30, 60));
        send_keyboard_report(0, 0,0,0,0,0,0);
        printf("Mac: Typo fix (teh->the)\n");
    }

    // 模拟手放回鼠标
    sleep_ms(rand_range(200, 500));
    // 鼠标微抖动联动
    send_mouse_report(0, rand_range(-2, 2), rand_range(-2, 2));
    return true;
}

// ========================================================================
// 键盘任务调度
// ========================================================================
static void keyboard_task(void) {
    if (!random_movement_enabled) {
        next_keyboard_interval = 0;
        return;
    }

    uint32_t now = board_millis();
    if (next_keyboard_interval == 0) {
        // 首次：2~5分钟后触发
        next_keyboard_interval = now + rand_range(120000, 300000);
        return;
    }

    if (now >= next_keyboard_interval) {
        keyboard_action_in_progress = true;
        generate_mac_keyboard_action();
        keyboard_action_in_progress = false;
        // 下次间隔 1.5~4.5 分钟
        next_keyboard_interval = now + rand_range(90000, 270000);
    }
}

// ========================================================================
// BOOTSEL 按键处理（短按切换启用）
// ========================================================================
static void bootsel_task(void) {
    static uint32_t last_poll_ms = 0;
    uint32_t now = board_millis();

    if (now - last_poll_ms < 10) {
        ws2812_status_task();
        return;
    }
    last_poll_ms = now;

    bool pressed = board_button_read() != 0;
    if (pressed && !bootsel_last_level) {
        if (now - bootsel_last_toggle_ms > 300) {
            random_movement_enabled = !random_movement_enabled;
            bootsel_last_toggle_ms = now;
            ws2812_flash_state(WS_LOG_ACTIVITY, 150);
            printf("Movement %s\n", random_movement_enabled ? "ENABLED" : "DISABLED");
        }
    }
    bootsel_last_level = pressed;
    ws2812_status_task();
}

// ========================================================================
// USB 描述符
// ========================================================================
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x046d, // Logitech
    .idProduct          = 0xc539, // G304
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

const uint8_t* tud_descriptor_device_cb(void) {
    return (uint8_t const*)&desc_device;
}

// 合并的 HID 报告描述符：鼠标 ID=1，键盘 ID=2
const uint8_t desc_hid_report[] = {
    // 鼠标报告 (ID=1)
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),

    // 键盘报告 (ID=2)
    HID_USAGE_PAGE_N( HID_USAGE_PAGE_DESKTOP, 1 ),
    HID_USAGE_N( HID_USAGE_DESKTOP_KEYBOARD, 1 ),
    HID_COLLECTION_N( HID_COLLECTION_APPLICATION, 1 ),
        HID_USAGE_N( HID_USAGE_DESKTOP_KEYBOARD, 1 ),
        HID_REPORT_ID( REPORT_ID_KEYBOARD ),
        HID_REPORT_SIZE( 8 ),
        HID_REPORT_COUNT( 1 ),
        HID_INPUT( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),
    HID_COLLECTION_END,
};

const uint8_t* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

// 配置描述符：2个接口 (鼠标 + 键盘)
uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0,
                          TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // 鼠标接口 (Interface 0)
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_MOUSE,
                       sizeof(desc_hid_report), 0x81,
                       CFG_TUD_HID_EP_BUFSIZE, 10),

    // 键盘接口 (Interface 1)
    TUD_HID_DESCRIPTOR(1, 0, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(desc_hid_report), 0x82,
                       CFG_TUD_HID_EP_BUFSIZE, 10),
};

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

// 字符串描述符
static const char* string_desc_arr[] = {
    "\x09\x04",              // 0: 语言 ID (0x0409)
    "Logitech",              // 1: 厂商
    "Logitech G304 + KB",    // 2: 产品
    "C539-001",              // 3: 序列号
};

const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc_str[20];
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr)/sizeof(string_desc_arr[0]))
            return NULL;
        const char* str = string_desc_arr[index];
        chr_count = strlen(str);
        if (chr_count > 19) chr_count = 19;
        for (uint8_t i = 0; i < chr_count; i++) {
            desc_str[1+i] = str[i];
        }
    }
    desc_str[0] = (TUSB_DESC_STRING << 8) | (2*chr_count + 2);
    return desc_str;
}

// HID 回调（必须实现，但无需额外操作）
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t* buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           const uint8_t* buffer, uint16_t bufsiz) {
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer; (void)bufsiz;
}

// ========================================================================
// USB 事件回调（更新 LED 状态）
// ========================================================================
void tud_mount_cb(void) {
    usb_host_mounted = true;
    usb_suspended = false;
    ws2812_restore_status_color();
    printf("USB Mounted\n");
}

void tud_umount_cb(void) {
    usb_host_mounted = false;
    usb_suspended = false;
    ws2812_restore_status_color();
    printf("USB Unmounted\n");
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    usb_suspended = true;
    ws2812_restore_status_color();
    printf("USB Suspended\n");
}

void tud_resume_cb(void) {
    usb_suspended = false;
    ws2812_restore_status_color();
    printf("USB Resumed\n");
}

// ========================================================================
// 主函数
// ========================================================================
int main(void) {
    board_init();

    if (!ws2812_init()) {
        ws2812_log_state(WS_LOG_ERROR);
        while (1) tight_loop_contents();
    }
    ws2812_log_state(WS_LOG_BOOTING);

    srand(to_ms_since_boot(get_absolute_time()));

    tusb_init();
    if (board_init_after_tusb) board_init_after_tusb();
    ws2812_restore_status_color();

    // 初始化键盘定时器
    next_keyboard_interval = 0;

    while (true) {
        tud_task();
        hid_task();
        keyboard_task();
        bootsel_task();
    }
}
