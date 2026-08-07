/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * 最终修复版 v3（完整回调）：复合 HID 鼠标 + 键盘
 * - 鼠标：贝塞尔 + 微震颤 + 过冲回调 + 随机点击（已修复释放）
 * - 键盘：8 种动作（非阻塞状态机）
 * - 工作/休息周期：活跃 5~15min，休息 2~5min
 * - LED 状态：琥珀启动，蓝 USB 准备，绿工作，琥珀休息，紫活动，黄挂起，红错误
 * - BOOTSEL 短按切换启用/禁用（修正极性）
 * - 上电默认禁用（红灯）
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

#define REPORT_ID_MOUSE    1
#define REPORT_ID_KEYBOARD 2

#define COLOR_RED          0xff0000
#define COLOR_GREEN        0x00ff00
#define COLOR_BLUE         0x0000ff
#define COLOR_AMBER        0xff8000
#define COLOR_YELLOW       0xffff00
#define COLOR_PURPLE       0xff00ff
#define COLOR_OFF          0x000000

// ========================================================================
// 全局变量
// ========================================================================
PIO ws2812_pio;
uint ws2812_sm;
uint ws2812_offset;

static bool usb_host_mounted = false;
static bool usb_suspended    = false;
static bool random_movement_enabled = false;

static bool ws_activity_flash = false;
static uint32_t ws_activity_timestamp = 0;
static uint32_t ws_activity_duration_ms = 0;

static bool bootsel_last_level = false;
static uint32_t bootsel_last_toggle_ms = 0;

// 工作/休息周期
typedef enum { MACRO_STATE_WORK, MACRO_STATE_REST } macro_state_t;
static macro_state_t macro_state = MACRO_STATE_WORK;
static uint32_t macro_until_ms = 0;

// 鼠标状态机
typedef enum {
    MOUSE_STATE_IDLE,
    MOUSE_STATE_MOVING,
    MOUSE_STATE_PAUSE,
    MOUSE_STATE_CLICK,
    MOUSE_STATE_CLICK_RELEASE
} mouse_state_t;
static mouse_state_t mouse_state = MOUSE_STATE_IDLE;
static uint32_t mouse_state_until = 0;
static int mouse_step = 0, mouse_total_steps = 0;
static float mouse_total_x = 0, mouse_total_y = 0;
static float mouse_cur_x = 0, mouse_cur_y = 0;
static uint32_t mouse_next_tick = 0;
static bool mouse_overshoot_done = false;

// 键盘状态机
typedef enum {
    KB_STATE_IDLE,
    KB_STATE_WAIT_START,
    KB_STATE_SEND_KEY,
    KB_STATE_SEND_MOD_KEY,
    KB_STATE_WAIT_INTERVAL,
    KB_STATE_FINISH
} keyboard_state_t;
typedef struct {
    keyboard_state_t state;
    uint32_t state_until_ms;
    uint8_t  step_index;
    uint8_t  total_steps;
    uint8_t  step_keys[8][2];
    uint16_t step_delays[8];
    bool     action_running;
} keyboard_context_t;
static keyboard_context_t kb_ctx = { .state = KB_STATE_IDLE, .action_running = false };
static uint32_t next_keyboard_interval_ms = 0;

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
static inline uint32_t rgb_to_grb(uint32_t rgb) {
    uint8_t r = (rgb >> 16) & 0xff;
    uint8_t g = (rgb >> 8) & 0xff;
    uint8_t b = rgb & 0xff;
    return (g << 16) | (r << 8) | b;
}
static inline uint32_t dim_color(uint32_t rgb, uint8_t shift) {
    uint8_t r = ((rgb >> 16) & 0xff) >> shift;
    uint8_t g = ((rgb >> 8) & 0xff) >> shift;
    uint8_t b = (rgb & 0xff) >> shift;
    return (r << 16) | (g << 8) | b;
}
void set_led_color(uint32_t rgb) {
    put_pixel(rgb_to_grb(rgb));
}

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
    if (random_movement_enabled && macro_state == MACRO_STATE_WORK) {
        set_led_color(dim_color(COLOR_GREEN, 1));
    } else if (random_movement_enabled && macro_state == MACRO_STATE_REST) {
        set_led_color(dim_color(COLOR_AMBER, 1));
    } else {
        set_led_color(dim_color(COLOR_RED, 1));
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

bool ws2812_init(void) {
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
// USB HID 报告发送
// ========================================================================
static void send_mouse_report(uint8_t buttons, int8_t dx, int8_t dy) {
    tud_hid_report(REPORT_ID_MOUSE, (uint8_t[]){ buttons, dx, dy, 0, 0 }, 5);
}
static void send_keyboard_report(uint8_t modifier, uint8_t k1, uint8_t k2,
                                 uint8_t k3, uint8_t k4, uint8_t k5, uint8_t k6) {
    uint8_t report[8] = { modifier, 0x00, k1, k2, k3, k4, k5, k6 };
    tud_hid_report(REPORT_ID_KEYBOARD, report, 8);
}

// ========================================================================
// 鼠标任务
// ========================================================================
static void hid_task(void) {
    if (!tud_hid_ready()) return;
    if (!random_movement_enabled) return;
    if (macro_state == MACRO_STATE_REST) return;

    uint32_t now = board_millis();
    if (now < mouse_next_tick) return;

    int8_t dx = 0, dy = 0;

    switch (mouse_state) {
        case MOUSE_STATE_IDLE:
            if (now > mouse_state_until) {
                mouse_total_x = rand_range(-200, 200);
                mouse_total_y = rand_range(-200, 200);
                if (rand() % 5 == 0) { mouse_total_x *= 2; mouse_total_y *= 2; }
                float dist = sqrtf(mouse_total_x * mouse_total_x + mouse_total_y * mouse_total_y);
                mouse_total_steps = (dist > 150) ? rand_range(20, 40) : rand_range(40, 100);
                mouse_cur_x = 0; mouse_cur_y = 0;
                mouse_step = 0;
                mouse_overshoot_done = false;
                mouse_state = MOUSE_STATE_MOVING;
                ws2812_log_state(WS_LOG_ACTIVITY);
            }
            break;

        case MOUSE_STATE_MOVING: {
            float t = (float)mouse_step / mouse_total_steps;
            float ease;
            if (t < 0.3) ease = 3 * t * t;
            else if (t < 0.7) ease = 0.5 + (t - 0.3);
            else ease = 1 - (1 - t) * (1 - t);

            float target_x = mouse_total_x * ease;
            float target_y = mouse_total_y * ease;

            if (!mouse_overshoot_done && t > 0.9 && rand() % 10 < 3) {
                float overshoot_factor = 1.0f + (rand_range(5, 15) / 100.0f);
                target_x = mouse_total_x * (ease * overshoot_factor);
                target_y = mouse_total_y * (ease * overshoot_factor);
                mouse_overshoot_done = true;
            }
            if (mouse_overshoot_done && t > 0.97) {
                target_x = mouse_total_x;
                target_y = mouse_total_y;
            }

            dx = (int)(target_x - mouse_cur_x);
            dy = (int)(target_y - mouse_cur_y);
            dx += (rand() % 3) - 1;
            dy += (rand() % 3) - 1;

            mouse_cur_x = target_x;
            mouse_cur_y = target_y;

            if (dx != 0 || dy != 0) {
                send_mouse_report(0, dx, dy);
            }
            mouse_step++;

            if (mouse_step >= mouse_total_steps) {
                if (rand() % 5 == 0) {
                    mouse_state = MOUSE_STATE_CLICK;
                    mouse_next_tick = now + 1;
                } else {
                    mouse_state = MOUSE_STATE_PAUSE;
                    mouse_state_until = now + rand_range(500, 3000);
                    mouse_next_tick = now + 10;
                }
            }

            if (t < 0.2)      mouse_next_tick = now + rand_range(2, 6);
            else if (t < 0.8) mouse_next_tick = now + rand_range(1, 4);
            else              mouse_next_tick = now + rand_range(5, 15);
            break;
        }

        case MOUSE_STATE_CLICK:
            send_mouse_report(0x01, 0, 0);
            mouse_state = MOUSE_STATE_CLICK_RELEASE;
            mouse_next_tick = now + rand_range(50, 120);
            break;

        case MOUSE_STATE_CLICK_RELEASE:
            send_mouse_report(0x00, 0, 0);
            mouse_state = MOUSE_STATE_PAUSE;
            mouse_state_until = now + rand_range(800, 4000);
            mouse_next_tick = now + 10;
            break;

        case MOUSE_STATE_PAUSE:
            if (now > mouse_state_until) {
                mouse_state = MOUSE_STATE_IDLE;
                mouse_state_until = now + rand_range(2000, 8000);
                ws2812_restore_status_color();
            }
            mouse_next_tick = now + rand_range(10, 30);
            break;
    }
}

// ========================================================================
// 键盘状态机（非阻塞）
// ========================================================================
static void kb_ctx_reset(void) {
    kb_ctx.state = KB_STATE_IDLE;
    kb_ctx.action_running = false;
    kb_ctx.step_index = 0;
    kb_ctx.total_steps = 0;
}
static void kb_add_step(uint8_t key, uint16_t delay) {
    if (kb_ctx.total_steps >= 8) return;
    kb_ctx.step_keys[kb_ctx.total_steps][0] = 0;
    kb_ctx.step_keys[kb_ctx.total_steps][1] = key;
    kb_ctx.step_delays[kb_ctx.total_steps] = delay;
    kb_ctx.total_steps++;
}
static void kb_add_mod_step(uint8_t mod, uint8_t key, uint16_t delay) {
    if (kb_ctx.total_steps >= 8) return;
    kb_ctx.step_keys[kb_ctx.total_steps][0] = mod;
    kb_ctx.step_keys[kb_ctx.total_steps][1] = key;
    kb_ctx.step_delays[kb_ctx.total_steps] = delay;
    kb_ctx.total_steps++;
}
static uint32_t kb_build_action(uint8_t action_type) {
    kb_ctx_reset();
    kb_add_step(0, rand_range(150, 400));
    switch (action_type) {
        case 0: kb_add_mod_step(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_S, rand_range(30,60)); kb_add_step(0,50); break;
        case 1: kb_add_mod_step(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_TAB, rand_range(60,120)); kb_add_mod_step(KEYBOARD_MODIFIER_LEFTGUI|KEYBOARD_MODIFIER_LEFTSHIFT, HID_KEY_TAB, rand_range(40,80)); kb_add_step(0,50); break;
        case 2: kb_add_mod_step(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_SPACE, rand_range(150,350)); kb_add_step(0, rand_range(100,200)); kb_add_step(HID_KEY_ESCAPE, rand_range(30,60)); kb_add_step(0,50); break;
        case 3: kb_add_step(HID_KEY_T, rand_range(60,120)); kb_add_step(0, rand_range(50,100)); kb_add_step(HID_KEY_E, rand_range(60,120)); kb_add_step(0, rand_range(50,100)); kb_add_step(HID_KEY_H, rand_range(60,120)); kb_add_step(0, rand_range(100,200)); kb_add_step(HID_KEY_BACKSPACE, rand_range(30,60)); kb_add_step(0, rand_range(50,100)); kb_add_step(HID_KEY_T, rand_range(60,120)); kb_add_step(0, rand_range(50,100)); kb_add_step(HID_KEY_H, rand_range(60,120)); kb_add_step(0, rand_range(50,100)); kb_add_step(HID_KEY_E, rand_range(60,120)); kb_add_step(0, rand_range(50,100)); kb_add_step(HID_KEY_SPACE, rand_range(30,60)); kb_add_step(0,50); break;
        case 4: kb_add_mod_step(KEYBOARD_MODIFIER_LEFTGUI, HID_KEY_W, rand_range(30,60)); kb_add_step(0,50); break;
        case 5: kb_add_step(HID_KEY_VOLUME_UP, rand_range(30,60)); kb_add_step(0,50); break;
        case 6: kb_add_step(HID_KEY_ARROW_DOWN, rand_range(80,200)); kb_add_step(0, rand_range(100,300)); kb_add_step(HID_KEY_ARROW_UP, rand_range(80,200)); kb_add_step(0,50); break;
        case 7: { int idx = rand()%26; uint8_t k=HID_KEY_A+idx; kb_add_step(k, rand_range(80,150)); kb_add_step(0, rand_range(100,250)); kb_add_step(HID_KEY_BACKSPACE, rand_range(30,60)); kb_add_step(0,50); break; }
        default: kb_add_step(0,50); break;
    }
    return 50;
}
static void keyboard_state_machine(void) {
    if (kb_ctx.state == KB_STATE_IDLE) return;
    uint32_t now = board_millis();
    if (now < kb_ctx.state_until_ms) return;
    switch (kb_ctx.state) {
        case KB_STATE_WAIT_START:
            kb_ctx.state = KB_STATE_SEND_KEY;
            kb_ctx.step_index = 0;
            kb_ctx.state_until_ms = now + 1;
            break;
        case KB_STATE_SEND_KEY: {
            if (kb_ctx.step_index >= kb_ctx.total_steps) {
                kb_ctx.state = KB_STATE_FINISH;
                kb_ctx.state_until_ms = now + 1;
                return;
            }
            uint8_t mod = kb_ctx.step_keys[kb_ctx.step_index][0];
            uint8_t key = kb_ctx.step_keys[kb_ctx.step_index][1];
            uint16_t delay = kb_ctx.step_delays[kb_ctx.step_index];
            if (mod != 0 || key != 0) {
                if (mod != 0 && key != 0) {
                    send_keyboard_report(mod, key, 0,0,0,0,0);
                    kb_ctx.state_until_ms = now + 20;
                    kb_ctx.state = KB_STATE_SEND_MOD_KEY;
                } else if (key != 0) {
                    send_keyboard_report(0, key, 0,0,0,0,0);
                    kb_ctx.state_until_ms = now + 20;
                    kb_ctx.state = KB_STATE_SEND_KEY;
                    kb_ctx.step_keys[kb_ctx.step_index][1] = 0;
                }
            } else {
                if (kb_ctx.step_index == 0 && delay > 50) {
                    kb_ctx.state_until_ms = now + delay;
                    kb_ctx.state = KB_STATE_WAIT_INTERVAL;
                } else {
                    send_keyboard_report(0, 0,0,0,0,0,0);
                    kb_ctx.step_index++;
                    kb_ctx.state_until_ms = now + delay;
                    kb_ctx.state = KB_STATE_WAIT_INTERVAL;
                }
            }
            break;
        }
        case KB_STATE_SEND_MOD_KEY: {
            uint8_t mod = kb_ctx.step_keys[kb_ctx.step_index][0];
            if (mod != 0) {
                send_keyboard_report(mod, 0,0,0,0,0,0);
                kb_ctx.state_until_ms = now + 20;
                kb_ctx.state = KB_STATE_SEND_KEY;
                kb_ctx.step_keys[kb_ctx.step_index][0] = 0;
                kb_ctx.step_keys[kb_ctx.step_index][1] = 0;
            }
            break;
        }
        case KB_STATE_WAIT_INTERVAL:
            kb_ctx.step_index++;
            kb_ctx.state = KB_STATE_SEND_KEY;
            kb_ctx.state_until_ms = now + 1;
            break;
        case KB_STATE_FINISH:
            send_mouse_report(0, rand_range(-2,2), rand_range(-2,2));
            kb_ctx_reset();
            next_keyboard_interval_ms = board_millis() + rand_range(90000, 270000);
            break;
        default: kb_ctx_reset(); break;
    }
}
static void keyboard_task(void) {
    if (!random_movement_enabled) { next_keyboard_interval_ms = 0; return; }
    if (macro_state == MACRO_STATE_REST) return;
    if (kb_ctx.action_running) { keyboard_state_machine(); return; }
    uint32_t now = board_millis();
    if (next_keyboard_interval_ms == 0) {
        next_keyboard_interval_ms = now + rand_range(120000, 300000);
        return;
    }
    if (now >= next_keyboard_interval_ms) {
        uint8_t action = rand() % 8;
        kb_build_action(action);
        kb_ctx.action_running = true;
        kb_ctx.state = KB_STATE_WAIT_START;
        kb_ctx.state_until_ms = now + 1;
    }
}

// ========================================================================
// 工作/休息周期
// ========================================================================
static void macro_cycle_task(void) {
    if (!random_movement_enabled) { macro_state = MACRO_STATE_WORK; return; }
    uint32_t now = board_millis();
    if (macro_until_ms == 0) {
        macro_state = MACRO_STATE_WORK;
        macro_until_ms = now + rand_range(300000, 900000);
        return;
    }
    if (now >= macro_until_ms) {
        if (macro_state == MACRO_STATE_WORK) {
            macro_state = MACRO_STATE_REST;
            macro_until_ms = now + rand_range(120000, 300000);
            ws2812_restore_status_color();
            mouse_state = MOUSE_STATE_IDLE;
            mouse_state_until = now + rand_range(2000, 8000);
            printf("Entering REST period\n");
        } else {
            macro_state = MACRO_STATE_WORK;
            macro_until_ms = now + rand_range(300000, 900000);
            ws2812_restore_status_color();
            printf("Entering WORK period\n");
        }
    }
}

// ========================================================================
// BOOTSEL 按键处理（修正极性）
// ========================================================================
// ========================================================================
// BOOTSEL 按键处理（修正上电误触 + 极性修正）
// ========================================================================
static void bootsel_task(void) {
    static uint32_t last_poll_ms = 0;
    static bool first_run = true;           // 首次运行标志，避免上电误触
    uint32_t now = board_millis();

    if (now - last_poll_ms < 10) {
        ws2812_status_task();
        return;
    }
    last_poll_ms = now;

    // Pico 的 BOOTSEL 引脚默认上拉，按下为低电平 (0)
    bool pressed = (board_button_read() == 0);

    // 第一次执行只记录电平，不触发任何切换
    if (first_run) {
        bootsel_last_level = pressed;
        first_run = false;
        // 恢复正确的 LED 状态（避免误操作导致的颜色错误）
        ws2812_restore_status_color();
        return;
    }

    // 检测下降沿（从释放到按下）
    if (pressed && !bootsel_last_level) {
        if (now - bootsel_last_toggle_ms > 300) {  // 防抖
            random_movement_enabled = !random_movement_enabled;
            bootsel_last_toggle_ms = now;
            ws2812_flash_state(WS_LOG_ACTIVITY, 150);
            printf("Movement %s\n", random_movement_enabled ? "ENABLED" : "DISABLED");

            if (random_movement_enabled) {
                macro_until_ms = 0;
                macro_state = MACRO_STATE_WORK;
            }
        }
    }
    bootsel_last_level = pressed;
    ws2812_status_task();
}

// ========================================================================
// TinyUSB 描述符和回调（必须全部实现）
// ========================================================================

// 设备描述符
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x046d,
    .idProduct          = 0xc539,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

const uint8_t* tud_descriptor_device_cb(void) {
    return (const uint8_t*)&desc_device;
}

// HID 报告描述符（鼠标 + 键盘复合）
const uint8_t desc_hid_report[] = {
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD))
};

const uint8_t* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

// 配置描述符（2 个接口）
uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0,
                          TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_MOUSE,
                       sizeof(desc_hid_report), 0x81,
                       CFG_TUD_HID_EP_BUFSIZE, 10),

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
    "\x09\x04",
    "Logitech",
    "Logitech G304 + KB",
    "C539-001",
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

// HID 回调（必须实现，即使空函数）
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

// USB 事件回调
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

    // 强制禁用，上电红灯
    random_movement_enabled = false;
    ws2812_restore_status_color();

    next_keyboard_interval_ms = 0;
    macro_until_ms = 0;
    kb_ctx_reset();

    while (true) {
        tud_task();
        macro_cycle_task();
        hid_task();
        keyboard_task();
        bootsel_task();
    }
}
