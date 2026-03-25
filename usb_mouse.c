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
        float ease = t * t * (3 - 2 * t);

        float target_x = total_x * ease;
        float target_y = total_y * ease;

        int dx = (int)(target_x - prev_x) + rand_range(-1, 1);
        int dy = (int)(target_y - prev_y) + rand_range(-1, 1);

        prev_x = target_x;
        prev_y = target_y;

        uint8_t report[4] = {0, (int8_t)dx, (int8_t)dy, 0};
        tud_hid_report(0, report, sizeof(report));

        uint8_t zero[4] = {0};
        tud_hid_report(0, zero, sizeof(zero));

        sleep_ms(rand_range(5, 15));
    }

    if (rand() % 25 == 0) {
        uint8_t click[4] = {1,0,0,0};
        tud_hid_report(0, click, sizeof(click));
        sleep_ms(20);
        uint8_t release[4] = {0};
        tud_hid_report(0, release, sizeof(release));
    }

    LED_ACTIVE;
}

// ─── USB 描述符 ───────────────────────

// HID report
static uint8_t const desc_hid_report[] = {
    0x05,0x01,0x09,0x02,0xA1,0x01,0x09,0x01,
    0xA1,0x00,0x05,0x09,0x19,0x01,0x29,0x03,
    0x15,0x00,0x25,0x01,0x95,0x03,0x75,0x01,
    0x81,0x02,0x95,0x01,0x75,0x05,0x81,0x03,
    0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,
    0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x03,
    0x81,0x06,0xC0,0xC0
};

static tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0xCafe,
    .idProduct = 0x4004,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

#define EPNUM_HID 0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1,1,0,CONFIG_TOTAL_LEN,TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,100),
    TUD_HID_DESCRIPTOR(0,0,HID_ITF_PROTOCOL_NONE,sizeof(desc_hid_report),EPNUM_HID,CFG_TUD_HID_EP_BUFSIZE,10)
};

static char const *string_desc_arr[] = {
    (const char[]){0x09,0x04},
    "Logitech",
    "USB Optical Mouse",
    "123456789"
};

static uint16_t _desc_str[32];

// ─── TinyUSB 回调 ─────────────────────
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
        _desc_str[1] = 0x0409;
        chr_count = 1;
    } else {
        const char *str = string_desc_arr[index];
        chr_count = strlen(str);
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1+i] = str[i];
        }
    }

    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2*chr_count+2);
    return _desc_str;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

uint16_t tud_hid_get_report_cb(uint8_t instance,uint8_t report_id,
                              hid_report_type_t report_type,
                              uint8_t *buffer,uint16_t reqlen) {
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance,uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,uint16_t bufsize) {}

// ─── 主程序 ──────────────────────────
int main(void) {
    stdio_init_all();

    ws2812_init_hw();
    LED_READY;

    srand(time_us_32() ^ rosc_hw->randombit);

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
