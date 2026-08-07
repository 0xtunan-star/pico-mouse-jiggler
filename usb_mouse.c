#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "tusb.h"

#define LED_PIN 25

#define REPORT_ID_MOUSE    1
#define REPORT_ID_KEYBOARD 2

// ------------------------------------------------------------
// HID 报告描述符
// ------------------------------------------------------------
const uint8_t desc_hid_report[] = {
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD))
};

// ------------------------------------------------------------
// 配置描述符：两个接口，无 IAD
// ------------------------------------------------------------
#define CFG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN)

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CFG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // 接口 0: 鼠标
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_MOUSE,
                       sizeof(desc_hid_report), 0x81,
                       CFG_TUD_HID_EP_BUFSIZE, 10),

    // 接口 1: 键盘
    TUD_HID_DESCRIPTOR(1, 0, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(desc_hid_report), 0x82,
                       CFG_TUD_HID_EP_BUFSIZE, 10),
};

// ------------------------------------------------------------
// 设备描述符
// ------------------------------------------------------------
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,          // 复合设备，由接口描述符定义
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

// ------------------------------------------------------------
// 回调函数
// ------------------------------------------------------------
const uint8_t* tud_descriptor_device_cb(void) {
    return (const uint8_t*)&desc_device;
}
const uint8_t* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}
uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

static const char* string_desc_arr[] = {
    "\x09\x04",
    "Logitech",
    "Pico Combo",
    "123456"
};
const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t str[20];
    if (index == 0) {
        memcpy(&str[1], string_desc_arr[0], 2);
        str[0] = 0x0304;
        return str;
    }
    if (index >= sizeof(string_desc_arr)/sizeof(char*)) return NULL;
    const char* s = string_desc_arr[index];
    uint8_t len = strlen(s);
    if (len > 18) len = 18;
    for (int i=0; i<len; i++) str[1+i] = s[i];
    str[0] = (TUSB_DESC_STRING << 8) | (2*len + 2);
    return str;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           const uint8_t* buffer, uint16_t bufsiz) {
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer; (void)bufsiz;
}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t* buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type;
    (void)buffer; (void)reqlen;
    return 0;
}

// ------------------------------------------------------------
// 发送鼠标报告
// ------------------------------------------------------------
static void send_mouse(int8_t dx, int8_t dy) {
    uint8_t report[5] = { 0, dx, dy, 0, 0 };
    tud_hid_report(REPORT_ID_MOUSE, report, 5);
}

// ------------------------------------------------------------
// 发送键盘报告
// ------------------------------------------------------------
static void send_keyboard(uint8_t modifier, uint8_t key) {
    uint8_t report[8] = { modifier, 0x00, key, 0, 0, 0, 0, 0 };
    tud_hid_report(REPORT_ID_KEYBOARD, report, 8);
}

// ------------------------------------------------------------
// 主函数
// ------------------------------------------------------------
int main(void) {
    board_init();
    tusb_init();

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);

    int step = 0;
    uint32_t last_key_time = 0;

    while (1) {
        tud_task();

        uint32_t now = to_ms_since_boot(get_absolute_time());

        // 闪烁 LED
        static uint32_t last_led = 0;
        if (now - last_led > 500) {
            gpio_put(LED_PIN, !gpio_get(LED_PIN));
            last_led = now;
        }

        // 鼠标画圈
        if (tud_hid_ready()) {
            int8_t dx = 3 * (step % 20 - 10) / 10;
            int8_t dy = 3 * (step % 15 - 7) / 10;
            send_mouse(dx, dy);
            step++;
            sleep_ms(20);
        }

        // 键盘每 5 秒按空格
        if (now - last_key_time > 5000) {
            if (tud_hid_ready()) {
                send_keyboard(0, HID_KEY_SPACE);
                sleep_ms(50);
                send_keyboard(0, 0);
                last_key_time = now;
            }
        }
    }
}
