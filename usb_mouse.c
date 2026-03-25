/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define MOUSE_BUTTON_LEFT 0x01
#include <stdio.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "ws2812.pio.h"

// WS2812 LED 接到 GP16
#define WS2812_PIN 16
#define NUM_PIXELS 1
#define IS_RGBW false

// 鼠标移动参数
#define MOVEMENT_INTERVAL_MS 600000  // 10分钟 = 600000毫秒
#define MAX_MOVEMENT 500
#define WS_ACTIVITY_FLASH_MS 50
#define WS_BOOTSEL_FLASH_MS 150
#define BOOTSEL_POLL_INTERVAL_MS 10
#define BOOTSEL_TOGGLE_GUARD_MS 300
#define SHAKE_MOVEMENT_COUNT 5  // 晃动时的移动次数
#define SHAKE_MOVEMENT_DELAY_MS 20  // 晃动时每次移动的间隔

// HID 配置
#define REPORT_ID_MOUSE 1

// BOOTSEL 长按定时（毫秒）
#define BOOTSEL_LONG_PRESS_MS 3000

// WS2812 相关全局变量
PIO ws2812_pio;
uint ws2812_sm;
uint ws2812_offset;
#define WS2812_FREQ 800000

// 鼠标移动状态
int8_t mouse_x = 0;
int8_t mouse_y = 0;
uint32_t last_movement_time = 0;
static bool shake_in_progress = false;
static uint32_t shake_start_time = 0;
static uint8_t shake_step = 0;

// 颜色常量（RGB 输入，函数中转换为 GRB）
#define COLOR_RED      0xff0000
#define COLOR_GREEN    0x00ff00
#define COLOR_BLUE     0x0000ff
#define COLOR_WHITE    0xffffff
#define COLOR_OFF      0x000000
#define COLOR_AMBER    0xff8000
#define COLOR_YELLOW   0xffff00
#define COLOR_PURPLE   0xff00ff
#define COLOR_RED_DIM  0x200000  // 低亮度红色，用于“未工作”提示
// 颜色含义：琥珀=启动、蓝=USB 准备、绿=主机连接、紫=活动、黄=挂起、红=错误

typedef enum {
    WS_LOG_BOOTING,
    WS_LOG_USB_READY,
    WS_LOG_HOST_READY,
    WS_LOG_ACTIVITY,
    WS_LOG_SUSPENDED,
    WS_LOG_ERROR
} ws_log_state_t;

static bool usb_host_mounted = false;
static bool usb_suspended = false;
static bool random_movement_enabled = false;
static bool bootsel_last_level = false;
static uint32_t bootsel_last_toggle_ms = 0;
static uint32_t ws_activity_duration_ms = 0;
static bool ws_activity_flash = false;
static uint32_t ws_activity_timestamp = 0;
static int rand_range(int min, int max) {
    return min + rand() % (max - min + 1);
}

static void hid_task(void)
{
    if (!tud_hid_ready()) return;

    uint32_t now = board_millis();

    // ================= 状态 =================
    static bool moving = false;
    static uint32_t last_action_time = 0;
    static uint32_t next_interval = 3000;

    static int step = 0;
    static int total_steps = 0;

    static float total_x = 0;
    static float total_y = 0;

    static float cur_x = 0;
    static float cur_y = 0;

    // ================= 开始新移动 =================
    if (!moving) {
        if (now - last_action_time < next_interval) return;

        // 新轨迹
        total_x = rand_range(-200, 200);
        total_y = rand_range(-150, 150);

        total_steps = rand_range(25, 60);

        cur_x = 0;
        cur_y = 0;
        step = 0;

        moving = true;
        return;
    }

    // ================= 执行轨迹 =================
    if (step < total_steps) {
        float t = (float)step / total_steps;

        // ⭐ 加速减速（核心）
        float ease;
        if (t < 0.5)
            ease = 2 * t * t;
        else
            ease = -1 + (4 - 2 * t) * t;

        float target_x = total_x * ease;
        float target_y = total_y * ease;

        int dx = (int)(target_x - cur_x);
        int dy = (int)(target_y - cur_y);

        cur_x = target_x;
        cur_y = target_y;

        // ⭐ 抖手
        dx += rand_range(-2, 2);
        dy += rand_range(-2, 2);

        if (dx != 0 || dy != 0) {
            tud_hid_mouse_report(REPORT_ID_MOUSE, 0, dx, dy, 0, 0);
        }

        step++;
        return;
    }

    // ================= 收尾（微抖 + 点击） =================
    for (int i = 0; i < rand_range(2, 5); i++) {
        int dx = rand_range(-3, 3);
        int dy = rand_range(-3, 3);
        tud_hid_mouse_report(REPORT_ID_MOUSE, 0, dx, dy, 0, 0);
    }

    // ⭐ 20% 概率点击
    if (rand() % 5 == 0) {
        tud_hid_mouse_report(REPORT_ID_MOUSE, MOUSE_BUTTON_LEFT, 0, 0, 0, 0);
        sleep_ms(50);
        tud_hid_mouse_report(REPORT_ID_MOUSE, 0, 0, 0, 0, 0);
    }

    // ================= 重置 =================
    moving = false;
    last_action_time = now;

    // 下次间隔（更像人）
    next_interval = rand_range(2000, 8000);
}

static inline void put_pixel(uint32_t pixel_grb) {
    pio_sm_put_blocking(ws2812_pio, ws2812_sm, pixel_grb << 8u);
}

static inline uint32_t rgb_to_grb(uint32_t rgb_color) {
    uint8_t r = (rgb_color >> 16) & 0xff;
    uint8_t g = (rgb_color >> 8) & 0xff;
    uint8_t b = rgb_color & 0xff;
    return (g << 16) | (r << 8) | b;
}

// 这是一个颜色变暗函数。它接收一个 24 位 RGB 颜色值（格式 0xRRGGBB），并将每个颜色通道右移 shift_bits 位，实现整体亮度的降低（变暗）。
// 例如 shift_bits=1 时亮度减半，shift_bits=2 时变为 1/4。
// 返回依然是 0xRRGGBB 格式的 dim 后的颜色。
static inline uint32_t dim_color(uint32_t rgb_color, uint8_t shift_bits) {
    uint8_t r = ((rgb_color >> 16) & 0xff) >> shift_bits; // 取出红色分量并右移
    uint8_t g = ((rgb_color >> 8) & 0xff) >> shift_bits;  // 取出绿色分量并右移
    uint8_t b = (rgb_color & 0xff) >> shift_bits;         // 取出蓝色分量并右移
    return (r << 16) | (g << 8) | b;                      // 合成为新的 RGB 值
}

bool ws2812_init() {
    printf("正在使用官方 PIO 程序初始化 GP%d 上的 WS2812...\n", WS2812_PIN);

    bool success = pio_claim_free_sm_and_add_program_for_gpio_range(
        &ws2812_program, &ws2812_pio, &ws2812_sm, &ws2812_offset, WS2812_PIN, 1, true);

    if (!success) {
        printf("申请 PIO 或状态机失败，WS2812 无法初始化\n");
        return false;
    }

    ws2812_program_init(ws2812_pio, ws2812_sm, ws2812_offset, WS2812_PIN, WS2812_FREQ, IS_RGBW);
    printf("WS2812 初始化完成（频率 %u Hz）\n", WS2812_FREQ);
    return true;
}

// 设置 LED 颜色（RGB 输入，函数内会转换成 GRB）
void set_led_color(uint32_t rgb_color) {
    uint32_t grb = rgb_to_grb(rgb_color);
    put_pixel(grb);
}

static void ws2812_log_state(ws_log_state_t state) {
    switch (state) {
            case WS_LOG_BOOTING:
                set_led_color(COLOR_AMBER);
                break;
            case WS_LOG_USB_READY:
                set_led_color(COLOR_BLUE);
                break;
        case WS_LOG_HOST_READY:
            set_led_color(COLOR_GREEN);
            break;
        case WS_LOG_ACTIVITY:
            set_led_color(COLOR_PURPLE);
            break;
        case WS_LOG_SUSPENDED:
            set_led_color(COLOR_YELLOW);
            break;
        case WS_LOG_ERROR:
        default:
            set_led_color(COLOR_RED);
            break;
    }
}

static void ws2812_restore_status_color(void) {
    if (usb_suspended) {
        ws2812_log_state(WS_LOG_SUSPENDED);
        return;
    }

    // 使用 usb_host_mounted 标志判断USB连接状态（在回调中设置，更稳定）
    // 不使用 tud_hid_ready() 因为它在USB协议栈处理间隙可能短暂返回false，导致闪烁
    if (!usb_host_mounted) {
        // USB未挂载，显示蓝色（USB准备状态）
        ws2812_log_state(WS_LOG_USB_READY);
        return;
    }

    // USB已挂载，根据工作状态显示颜色：关闭时红色，开启时绿色
    if (random_movement_enabled) {
        set_led_color(dim_color(COLOR_GREEN, 1));  // 开启时一直显示绿色
    } else {
        set_led_color(dim_color(COLOR_RED, 1));  // 关闭时显示较暗的红色
    }
}

static void ws2812_flash_state(ws_log_state_t state, uint32_t duration_ms) {
    ws2812_log_state(state);
    ws_activity_flash = true;
    ws_activity_timestamp = board_millis();
    ws_activity_duration_ms = duration_ms;
}

static void ws2812_status_task(void) {
    if (!ws_activity_flash) {
        return;
    }

    if (board_millis() - ws_activity_timestamp >= ws_activity_duration_ms) {
        ws_activity_flash = false;
        ws2812_restore_status_color();
    }
}

static int8_t random_delta(void) {
    int delta;
    do {
        delta = (int)(rand() % (2 * MAX_MOVEMENT + 1)) - MAX_MOVEMENT;
    } while (delta == 0);
    return (int8_t)delta;
}

// 触发鼠标晃动
void trigger_mouse_shake(void) {
    shake_in_progress = true;
    shake_start_time = board_millis();
    shake_step = 0;
    random_movement_enabled = true;  // 启用自动移动功能
    last_movement_time = board_millis() - MOVEMENT_INTERVAL_MS;  // 重置定时器
}

// 处理鼠标晃动
bool process_mouse_shake(void) {
    if (!shake_in_progress) {
        return false;
    }

    uint32_t now = board_millis();
    
    // 检查是否到了下一次晃动步骤的时间
    if (now - shake_start_time < shake_step * SHAKE_MOVEMENT_DELAY_MS) {
        return false;
    }

    if (shake_step < SHAKE_MOVEMENT_COUNT) {
        // 生成晃动移动：小范围的随机移动
        int8_t shake_x = (int8_t)((rand() % 7) - 3);  // -3 到 3
        int8_t shake_y = (int8_t)((rand() % 7) - 3);  // -3 到 3
        
        // 确保至少有一个方向有移动
        if (shake_x == 0 && shake_y == 0) {
            shake_x = 1;
        }
        
        mouse_x = shake_x;
        mouse_y = shake_y;
        shake_step++;
        return true;
    } else {
        // 晃动完成
        shake_in_progress = false;
        return false;
    }
}

// 鼠标移动生成器（每10分钟移动1像素，防止息屏）
bool generate_movement(void) {
    uint32_t now = board_millis();

    if (!random_movement_enabled) return false;
    if (shake_in_progress) return false;

    // 👉 3~8秒触发一次（方便你测试）
    static uint32_t next_interval = 0;
    if (next_interval == 0) {
        next_interval = rand_range(3000, 8000);
    }

    if (now - last_movement_time < next_interval) {
        return false;
    }

    // ====== 开始拟真人类移动 ======
    ws2812_log_state(WS_LOG_ACTIVITY); // 紫色

    int total_x = rand_range(-120, 120);
    int total_y = rand_range(-120, 120);

    int steps = rand_range(20, 40);

    float cur_x = 0;
    float cur_y = 0;

    for (int i = 0; i < steps; i++) {
        float t = (float)i / steps;

        // 👉 加速/减速曲线（关键）
        float ease;
        if (t < 0.5) ease = 2 * t * t;
        else ease = -1 + (4 - 2 * t) * t;

        float target_x = total_x * ease;
        float target_y = total_y * ease;

        int move_x = (int)(target_x - cur_x);
        int move_y = (int)(target_y - cur_y);

        cur_x = target_x;
        cur_y = target_y;

        // 👉 抖手
        move_x += rand_range(-2, 2);
        move_y += rand_range(-2, 2);

        if (move_x == 0 && move_y == 0) continue;

        tud_hid_mouse_report(REPORT_ID_MOUSE, 0, move_x, move_y, 0, 0);

        // 👉 随机停顿（更像人）
        if (rand() % 10 == 0) {
            sleep_ms(rand_range(80, 200));
        } else {
            sleep_ms(rand_range(10, 25));
        }
    }

    // 👉 收尾微抖
    for (int i = 0; i < rand_range(2, 5); i++) {
        int dx = rand_range(-5, 5);
        int dy = rand_range(-5, 5);

        tud_hid_mouse_report(REPORT_ID_MOUSE, 0, dx, dy, 0, 0);
        sleep_ms(rand_range(20, 60));
    }

    sleep_ms(300); // 👉 保持紫色一会

    ws2812_restore_status_color(); // 恢复绿灯

    last_movement_time = now;
    next_interval = rand_range(3000, 8000);

    return true;
}

static void bootsel_task(void) {
    static uint32_t last_poll_ms = 0;
    uint32_t now = board_millis();

    if (now - last_poll_ms < BOOTSEL_POLL_INTERVAL_MS) {
        ws2812_status_task();
        return;
    }
    last_poll_ms = now;

    bool pressed = board_button_read() != 0;

    if (pressed && !bootsel_last_level) {
        if (now - bootsel_last_toggle_ms > BOOTSEL_TOGGLE_GUARD_MS) {
            // 切换开关状态
            if (random_movement_enabled) {
                // 关闭：停止晃动和自动移动
                random_movement_enabled = false;
                shake_in_progress = false;
            } else {
                // 开启：触发晃动并启用自动移动
                trigger_mouse_shake();
            }
            bootsel_last_toggle_ms = now;

            ws2812_flash_state(WS_LOG_ACTIVITY, WS_BOOTSEL_FLASH_MS);
        }
    }

    bootsel_last_level = pressed;
    ws2812_status_task();
}



int main(void) {
    // 初始化板级外设（GPIO、电源等）
    board_init();

    // 初始化 WS2812，失败则停机并保持红灯
    if (!ws2812_init()) {
        ws2812_log_state(WS_LOG_ERROR);
        while (1) {
            tight_loop_contents();
        }
    }
    ws2812_log_state(WS_LOG_BOOTING);

    // 用系统时间作为随机数种子
    srand(to_ms_since_boot(get_absolute_time()));

    // 初始化 TinyUSB，若板卡需要额外设置则在其后执行
    tusb_init();
    if (board_init_after_tusb) {
        board_init_after_tusb();
    }
    ws2812_restore_status_color();

    // 主循环：维持 TinyUSB、随机鼠标和 Bootsel 控制
    while (true) {
        tud_task();       // USB 协议栈
        hid_task();       // HID 报告生成
        bootsel_task();   // Bootsel 轮询用于启停随机移动
    }
}

// USB 设备事件回调
void tud_mount_cb(void) {
    printf("\n>>> USB Mounted - LED GREEN <<<\n");
    usb_host_mounted = true;
    usb_suspended = false;
    ws2812_restore_status_color();
}

void tud_umount_cb(void) {
    printf("\n>>> USB Unmounted - LED BLUE <<<\n");
    usb_host_mounted = false;
    usb_suspended = false;
    ws2812_restore_status_color();
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void) remote_wakeup_en;
    printf("\n>>> USB Suspended - LED YELLOW <<<\n");
    usb_suspended = true;
    ws2812_restore_status_color();
}

void tud_resume_cb(void) {
    printf("\n>>> USB Resumed - LED GREEN <<<\n");
    usb_suspended = false;
    ws2812_restore_status_color();
}

// TinyUSB 描述符回调
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x046d, // Logitech
    .idProduct          = 0xc539, // G304 Lightspeed Receiver
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

const uint8_t* tud_descriptor_device_cb(void) {
    return (uint8_t const*) &desc_device;
}

// HID 报告描述符
const uint8_t desc_hid_report[] = {
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE))
};

const uint8_t* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void) instance;
    return desc_hid_report;
}

// 配置描述符
uint8_t const desc_configuration[] = {
    // 配置编号、接口数量、字符串索引、总长、属性、供电 mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // 接口编号、字符串索引、协议、报告描述符长度、端点地址、容量与轮询周期
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(desc_hid_report), 0x81, CFG_TUD_HID_EP_BUFSIZE, 10)
};

uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void) index;
    return desc_configuration;
}

// 字符串描述符
const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void) langid;

    #define DESC_STR_MAX (20)
    static uint16_t desc_str[DESC_STR_MAX];

    static char const* string_desc_arr[] = {
        "\x09\x04",                      // 0：语言 ID（英文 0x0409）
        "Logitech",                      // 1：厂商
        "Logitech G304",                 // 2：产品名
        "C539-001",                      // 3：序列号
    };

    uint8_t chr_count;

    if (index == 0) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (!(index < sizeof(string_desc_arr)/sizeof(string_desc_arr[0]))) return NULL;

        const char* str = string_desc_arr[index];

        chr_count = strlen(str);
        if ( chr_count > (DESC_STR_MAX-1) ) chr_count = DESC_STR_MAX-1;

        for(uint8_t i=0; i<chr_count; i++) {
            desc_str[1+i] = str[i];
        }
    }

    desc_str[0] = (TUSB_DESC_STRING << 8 ) | (2*chr_count + 2);

    return desc_str;
}

// HID 回调
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsiz) {
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) bufsiz;
}
