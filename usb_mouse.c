/*
 * pico-mouse-jiggler (Enhanced Human Version)
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/structs/rosc.h"

#include "tusb.h"
#include "ws2812.pio.h"

// ─── WS2812 ─────────────────────────────
#define WS2812_PIN  16
#define WS2812_FREQ 800000

static PIO ws_pio = pio0;
static uint ws_sm = 0;

static void ws2812_init_hw(void) {
    uint offset = pio_add_program(ws_pio, &ws2812_program);
    ws2812_program_init(ws_pio, ws_sm, offset, WS2812_PIN, WS2812_FREQ, false);
}

static void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t grb = ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8);
    pio_sm_put_blocking(ws_pio, ws_sm, grb);
}

#define LED_ACTIVE   ws2812_set_color(0,255,0)
#define LED_MOVING   ws2812_set_color(128,0,128)
#define LED_READY    ws2812_set_color(0,0,255)
#define LED_OFF      ws2812_set_color(0,0,0)

// ─── BOOTSEL ───────────────────────────
#define BOOTSEL_PIN 23
#define DEBOUNCE_MS 300

static bool jiggle_enabled = true;
static uint32_t last_press = 0;

// ─── 随机 ─────────────────────────────
static int rand_range(int min, int max) {
    return min + (rand() % (max - min + 1));
}

// ─── 行为模型 ─────────────────────────
typedef enum {
    MOVE_MICRO,
    MOVE_SHORT,
    MOVE_MEDIUM
} move_type_t;

static move_type_t pick_move_type(void) {
    int r = rand() % 100;
    if (r < 60) return MOVE_MICRO;
    else if (r < 90) return MOVE_SHORT;
    else return MOVE_MEDIUM;
}

// ─── 时间模型（无长时间停顿） ─────────
static uint32_t next_interval(void) {
    int r = rand() % 100;

    if (r < 70) return rand_range(5000, 12000);
    else return rand_range(12000, 25000);
}

// ─── 拟人移动 ─────────────────────────
static void do_human_move(void) {
    if (!tud_hid_ready()) return;

    LED_MOVING;

    move_type_t type = pick_move_type();

    int total_x, total_y, steps;

    switch (type) {
        case MOVE_MICRO:
            total_x = rand_range(-5, 5);
            total_y = rand_range(-5, 5);
            steps   = rand_range(5, 10);
            break;
        case MOVE_SHORT:
            total_x = rand_range(-30, 30);
            total_y = rand_range(-30, 30);
            steps   = rand_range(10, 20);
            break;
        default:
            total_x = rand_range(-80, 80);
            total_y = rand_range(-80, 80);
            steps   = rand_range(20, 35);
            break;
    }

    float prev_x = 0;
    float prev_y = 0;

    for (int i = 1; i <= steps; i++) {
        float t = (float)i / steps;

        // easing（人类加速曲线）
        float ease = t * t * (3 - 2 * t);

        float target_x = total_x * ease;
        float target_y = total_y * ease;

        int dx = (int)(target_x - prev_x) + rand_range(-1, 1);
        int dy = (int)(target_y - prev_y) + rand_range(-1, 1);

        prev_x = target_x;
        prev_y = target_y;

        uint8_t report[4] = {0, (int8_t)dx, (int8_t)dy, 0};
        tud_hid_report(0, report, sizeof(report));

        // 归零帧（关键）
        uint8_t zero[4] = {0};
        tud_hid_report(0, zero, sizeof(zero));

        sleep_ms(rand_range(5, 15));
    }

    // 偶尔点击（更像人）
    if (rand() % 25 == 0) {
        uint8_t click[4] = {1,0,0,0};
        tud_hid_report(0, click, sizeof(click));
        sleep_ms(20);
        uint8_t release[4] = {0};
        tud_hid_report(0, release, sizeof(release));
    }

    LED_ACTIVE;
}

// ─── USB 描述符（伪装） ───────────────
static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "Logitech",
    "USB Optical Mouse",
    "123456789"
};

// 其他 USB 描述符（保持你原来的，不变）
// 👉 这里省略（你原代码直接保留）

// ─── 主程序 ──────────────────────────
int main(void) {
    stdio_init_all();

    ws2812_init_hw();
    LED_READY;

    // 随机增强
    srand(time_us_32() ^ rosc_hw->randombit);

    // BOOTSEL 初始化（修复点）
    gpio_init(BOOTSEL_PIN);
    gpio_set_dir(BOOTSEL_PIN, GPIO_IN);
    gpio_pull_up(BOOTSEL_PIN);

    tusb_init();

    while (!tud_mounted()) {
        tud_task();
        sleep_ms(10);
    }

    LED_ACTIVE;

    uint32_t next_jiggle_ms = next_interval();
    uint32_t last_jiggle = to_ms_since_boot(get_absolute_time());

    while (true) {
        tud_task();

        uint32_t now = to_ms_since_boot(get_absolute_time());

        // 按键切换
        bool btn_pressed = !gpio_get(BOOTSEL_PIN);
        if (btn_pressed && (now - last_press) > DEBOUNCE_MS) {
            last_press = now;
            jiggle_enabled = !jiggle_enabled;
        }

        if (jiggle_enabled && tud_mounted()) {
            if ((now - last_jiggle) >= next_jiggle_ms) {
                do_human_move();
                last_jiggle = now;
                next_jiggle_ms = next_interval();
            }
        }

        sleep_ms(5);
    }
}
