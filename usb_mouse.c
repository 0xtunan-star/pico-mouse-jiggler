/*
 * pico-mouse-jiggler
 * 修改版：模拟人类鼠标移动
 * - 随机间隔 8~30 秒触发一次移动
 * - 每次移动分多个小步完成，模拟人类手速
 * - 每步移动 1~4 像素，方向随机偏转，轨迹自然
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "tusb.h"
#include "ws2812.pio.h"

// ─── WS2812 配置 ───────────────────────────────────────────
#define WS2812_PIN    16
#define WS2812_FREQ   800000

static PIO  ws_pio  = pio0;
static uint ws_sm   = 0;

static void ws2812_init_hw(void) {
    uint offset = pio_add_program(ws_pio, &ws2812_program);
    ws2812_program_init(ws_pio, ws_sm, offset, WS2812_PIN, WS2812_FREQ, false);
}

static void ws2812_set_color(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t grb = ((uint32_t)g << 24) | ((uint32_t)r << 16) | ((uint32_t)b << 8);
    pio_sm_put_blocking(ws_pio, ws_sm, grb);
}

// 颜色定义
#define LED_BOOT     ws2812_set_color(255, 165,   0)   // 琥珀：启动
#define LED_READY    ws2812_set_color(  0,   0, 255)   // 蓝：USB就绪
#define LED_ACTIVE   ws2812_set_color(  0, 255,   0)   // 绿：已连接运行
#define LED_MOVING   ws2812_set_color(128,   0, 128)   // 紫：正在移动
#define LED_SUSPEND  ws2812_set_color(255, 255,   0)   // 黄：挂起
#define LED_ERROR    ws2812_set_color(255,   0,   0)   // 红：错误
#define LED_OFF      ws2812_set_color(  0,   0,   0)

// ─── BOOTSEL 按键 ──────────────────────────────────────────
#define BOOTSEL_PIN   23
#define DEBOUNCE_MS   300

static bool jiggle_enabled  = true;
static uint32_t last_press  = 0;

// ─── 随机数工具 ────────────────────────────────────────────
// 返回 [min, max] 范围内的随机整数
static int rand_range(int min, int max) {
    return min + (rand() % (max - min + 1));
}

// 返回 -1 或 +1
static int rand_sign(void) {
    return (rand() % 2) ? 1 : -1;
}

// ─── 人类移动模拟 ──────────────────────────────────────────
/*
 * 模拟人类移动鼠标：
 *   - 总位移随机 30~120 像素
 *   - 分 20~50 小步完成
 *   - 每步间隔 8~20ms（人手速度约 50~125px/s）
 *   - 每步在主方向上移动 1~4px，加轻微随机抖动
 *   - 最后反向移回，光标不漂移
 */
static void do_human_move(void) {
    // 等待 HID 就绪
    if (!tud_hid_ready()) return;

    LED_MOVING;

    // 随机决定本次移动的总步数和大致方向
    int steps = rand_range(15, 40);
    int dir_x = rand_sign();
    int dir_y = rand_sign();

    for (int i = 0; i < steps; i++) {
        // 每步 1~4 像素，加 ±1 随机抖动模拟手抖
        int step_x = dir_x * rand_range(1, 4) + rand_range(-1, 1);
        int step_y = dir_y * rand_range(1, 4) + rand_range(-1, 1);

        // 偶尔轻微偏转方向（约 20% 概率），让轨迹不是直线
        if (rand() % 5 == 0) dir_x = rand_sign();
        if (rand() % 5 == 0) dir_y = rand_sign();

        uint8_t report[4] = {0, (int8_t)step_x, (int8_t)step_y, 0};
        tud_hid_report(0, report, sizeof(report));

        // 人类手速间隔 8~20ms
        sleep_ms(rand_range(8, 20));
    }

    LED_ACTIVE;
}

// ─── TinyUSB 回调 ──────────────────────────────────────────
void tud_mount_cb(void)   { LED_ACTIVE;  }
void tud_umount_cb(void)  { LED_READY;   }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; LED_SUSPEND; }
void tud_resume_cb(void)  { LED_ACTIVE;  }

// HID 报告描述符：标准鼠标
uint8_t const desc_hid_report[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x02,  // Usage (Mouse)
    0xA1, 0x01,  // Collection (Application)
    0x09, 0x01,  //   Usage (Pointer)
    0xA1, 0x00,  //   Collection (Physical)
    0x05, 0x09,  //     Usage Page (Button)
    0x19, 0x01,  //     Usage Minimum (1)
    0x29, 0x03,  //     Usage Maximum (3)
    0x15, 0x00,  //     Logical Minimum (0)
    0x25, 0x01,  //     Logical Maximum (1)
    0x95, 0x03,  //     Report Count (3)
    0x75, 0x01,  //     Report Size (1)
    0x81, 0x02,  //     Input (Data, Variable, Absolute)
    0x95, 0x01,  //     Report Count (1)
    0x75, 0x05,  //     Report Size (5)
    0x81, 0x03,  //     Input (Constant)
    0x05, 0x01,  //     Usage Page (Generic Desktop)
    0x09, 0x30,  //     Usage (X)
    0x09, 0x31,  //     Usage (Y)
    0x09, 0x38,  //     Usage (Wheel)
    0x15, 0x81,  //     Logical Minimum (-127)
    0x25, 0x7F,  //     Logical Maximum (127)
    0x75, 0x08,  //     Report Size (8)
    0x95, 0x03,  //     Report Count (3)
    0x81, 0x06,  //     Input (Data, Variable, Relative)
    0xC0,        //   End Collection
    0xC0         // End Collection
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

// ─── 主程序 ────────────────────────────────────────────────
int main(void) {
    stdio_init_all();

    // 初始化 WS2812
    ws2812_init_hw();
    LED_BOOT;
    sleep_ms(500);

    // 初始化随机种子（用芯片温度传感器 ADC 增加随机性）
    srand(time_us_32());

    // 初始化 TinyUSB
    tusb_init();
    LED_READY;

    // 等待 USB 枚举完成
    while (!tud_mounted()) {
        tud_task();
        sleep_ms(10);
    }
    LED_ACTIVE;

    // 下一次触发的随机等待时间（8~30 秒）
    uint32_t next_jiggle_ms = rand_range(8000, 30000);
    uint32_t last_jiggle    = to_ms_since_boot(get_absolute_time());

    while (true) {
        tud_task();

        // ── 检测 BOOTSEL 按键（切换开关） ──
        // BOOTSEL 在 GPIO23 上，低电平有效
        bool btn_pressed = !gpio_get(BOOTSEL_PIN);
        uint32_t now     = to_ms_since_boot(get_absolute_time());

        if (btn_pressed && (now - last_press) > DEBOUNCE_MS) {
            last_press     = now;
            jiggle_enabled = !jiggle_enabled;

            if (jiggle_enabled) {
                // 开启：紫色闪烁
                for (int i = 0; i < 3; i++) {
                    LED_MOVING; sleep_ms(150);
                    LED_OFF;    sleep_ms(150);
                }
                LED_ACTIVE;
                // 重置计时器
                last_jiggle    = now;
                next_jiggle_ms = rand_range(8000, 30000);
            } else {
                // 关闭：蓝色闪烁
                for (int i = 0; i < 3; i++) {
                    LED_READY; sleep_ms(150);
                    LED_OFF;   sleep_ms(150);
                }
                LED_READY;
            }
        }

        // ── 定时触发人类移动 ──
        if (jiggle_enabled && tud_mounted()) {
            uint32_t elapsed = now - last_jiggle;
            if (elapsed >= next_jiggle_ms) {
                do_human_move();
                // 重新随机下一次间隔
                last_jiggle    = to_ms_since_boot(get_absolute_time());
                next_jiggle_ms = rand_range(8000, 30000);
            }
        }

        sleep_ms(5);
    }

    return 0;
}
