#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "tusb.h"

// 板载 LED（Pico 的 GPIO 25）
#define LED_PIN 25

// 报告 ID
#define REPORT_ID_MOUSE    1
#define REPORT_ID_KEYBOARD 2

// ------------------------------------------------------------
// HID 报告描述符（鼠标 + 键盘）
// ------------------------------------------------------------
const uint8_t desc_hid_report[] = {
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD))
};

// ------------------------------------------------------------
// 配置描述符（2 个接口）
// ------------------------------------------------------------
uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0,
                          TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    // 鼠标接口
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_MOUSE,
                       sizeof(desc_hid_report), 0x81,
                       CFG_TUD_HID_EP_BUFSIZE, 10),
    // 键盘接口
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

// ------------------------------------------------------------
// 回调函数（必须实现）
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

// 字符串描述符
static const char* string_desc_arr[] = {
    "\x09\x04",          // 语言 ID
    "Logitech",
    "Pico Mouse+KB",
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

// HID 回调（空实现）
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
// 发送鼠标报告（dx, dy）
// ------------------------------------------------------------
static void send_mouse(int8_t dx, int8_t dy) {
    uint8_t report[5] = { 0, dx, dy, 0, 0 };
    tud_hid_report(REPORT_ID_MOUSE, report, 5);
}

// ------------------------------------------------------------
// 发送键盘报告（按下/释放按键）
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

    // 初始化板载 LED
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    int step = 0;
    uint32_t last_key_time = 0;
    bool key_pressed = false;

    while (1) {
        tud_task();  // 必须不断调用

        // 心跳 LED（闪烁指示运行）
        static uint32_t last_led_toggle = 0;
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_led_toggle > 500) {
            gpio_put(LED_PIN, !gpio_get(LED_PIN));
            last_led_toggle = now;
        }

        // 鼠标：画圈（如果 HID 准备就绪）
        if (tud_hid_ready()) {
            // 简单的圆形轨迹
            int8_t dx = 3 * (step % 20 - 10) / 10;
            int8_t dy = 3 * (step % 15 - 7) / 10;
            send_mouse(dx, dy);
            step++;
            sleep_ms(20);
        }

        // 键盘：每 5 秒按一次空格
        if (now - last_key_time > 5000) {
            if (tud_hid_ready()) {
                send_keyboard(0, HID_KEY_SPACE);  // 按下
                sleep_ms(50);
                send_keyboard(0, 0);              // 释放
                last_key_time = now;
            }
        }
    }
}
