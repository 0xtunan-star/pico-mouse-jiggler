/*
 * pico-mouse-jiggler
 * 修改版：模拟人类鼠标移动
 * - 随机间隔 8~30 秒触发一次移动
 * - 每次移动分多个小步完成，模拟人类手速
 * - 每步 1~4 像素，方向随机偏转，不归位
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

#define LED_BOOT     ws2812_set_color(255, 165,   0)
#define LED_READY    ws2812_set_color(  0,   0, 255)
#define LED_ACTIVE   ws2812_set_color(  0, 255,   0)
#define LED_MOVING   ws2812_set_color(128,   0, 128)
#define LED_SUSPEND  ws2812_set_color(255, 255,   0)
#define LED_OFF      ws2812_set_color(  0,   0,   0)

// ─── BOOTSEL 按键 ──────────────────────────────────────────
#define BOOTSEL_PIN   23
#define DEBOUNCE_MS   300

static bool jiggle_enabled = true;
static uint32_t last_press = 0;

// ─── 随机数工具 ────────────────────────────────────────────
static int rand_range(int min, int max) {
    return min + (rand() % (max - min + 1));
}
static int rand_sign(void) {
    return (rand() % 2) ? 1 : -1;
}

// ─── 人类移动模拟 ──────────────────────────────────────────
static void do_human_move(void) {
    if (!tud_hid_ready()) return;
    LED_MOVING;

    int steps = rand_range(15, 40);
    int dir_x = rand_sign();
    int dir_y = rand_sign();

    for (int i = 0; i < steps; i++) {
        int step_x = dir_x * rand_range(1, 4) + rand_range(-1, 1);
        int step_y = dir_y * rand_range(1, 4) + rand_range(-1, 1);
        if (rand() % 5 == 0) dir_x = rand_sign();
        if (rand() % 5 == 0) dir_y = rand_sign();
        uint8_t report[4] = {0, (int8_t)step_x, (int8_t)step_y, 0};
        tud_hid_report(0, report, sizeof(report));
        sleep_ms(rand_range(8, 20));
    }

    LED_ACTIVE;
}

// ─── USB 描述符 ────────────────────────────────────────────

// HID 报告描述符
static uint8_t const desc_hid_report[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01,
    0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38,
    0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03,
    0x81, 0x06, 0xC0, 0xC0
};

// Device 描述符
static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0xCafe,
    .idProduct          = 0x4004,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

// Configuration 描述符
#define EPNUM_HID     0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, 10)
};

// String 描述符
static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},  // 0: 语言
    "PicoMouse",                  // 1: 厂商
    "Mouse Jiggler",              // 2: 产品
    "000001",                     // 3: 序列号
};
static uint16_t _desc_str[32];

// ─── TinyUSB 回调 ──────────────────────────────────────────
uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;
    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = str[i];
    }
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

void tud_mount_cb(void)   { LED_ACTIVE;  }
void tud_umount_cb(void)  { LED_READY;   }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; LED_SUSPEND; }
void tud_resume_cb(void)  { LED_ACTIVE;  }

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
    ws2812_init_hw();
    LED_BOOT;
    sleep_ms(500);

    srand(time_us_32());
    tusb_init();
    LED_READY;

    while (!tud_mounted()) {
        tud_task();
        sleep_ms(10);
    }
    LED_ACTIVE;

    uint32_t next_jiggle_ms = rand_range(8000, 30000);
    uint32_t last_jiggle    = to_ms_since_boot(get_absolute_time());

    while (true) {
        tud_task();
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // BOOTSEL 按键切换开关
        bool btn_pressed = !gpio_get(BOOTSEL_PIN);
        if (btn_pressed && (now - last_press) > DEBOUNCE_MS) {
            last_press     = now;
            jiggle_enabled = !jiggle_enabled;
            if (jiggle_enabled) {
                for (int i = 0; i < 3; i++) { LED_MOVING; sleep_ms(150); LED_OFF; sleep_ms(150); }
                LED_ACTIVE;
                last_jiggle    = now;
                next_jiggle_ms = rand_range(8000, 30000);
            } else {
                for (int i = 0; i < 3; i++) { LED_READY; sleep_ms(150); LED_OFF; sleep_ms(150); }
                LED_READY;
            }
        }

        // 定时触发移动（8~30 秒随机间隔）
        if (jiggle_enabled && tud_mounted()) {
            if ((now - last_jiggle) >= next_jiggle_ms) {
                do_human_move();
                last_jiggle    = to_ms_since_boot(get_absolute_time());
                next_jiggle_ms = rand_range(8000, 30000);
            }
        }

        sleep_ms(5);
    }

    return 0;
}
