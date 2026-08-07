#include <stdio.h>
#include "pico/stdlib.h"
#include "tusb.h"

//--------------------------------------------------------------------+
// HID Report Descriptors (两个接口独立)
//--------------------------------------------------------------------+
static const uint8_t desc_mouse_report[] = {
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(1))
};

static const uint8_t desc_keyboard_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1))
};

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(desc_mouse_report), 0x81, 64, 10),
    TUD_HID_DESCRIPTOR(1, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(desc_keyboard_report), 0x82, 64, 10),
};

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+
static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x046D,
    .idProduct = 0xC539,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
static const char *string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "Logitech",
    "Pico Combo",
    "123456"
};

//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+
const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&desc_device; }
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return desc_configuration; }
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t str[32];
    if (index == 0) { memcpy(&str[1], string_desc_arr[0], 2); str[0] = 0x0304; return str; }
    if (index >= sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) return NULL;
    const char *s = string_desc_arr[index];
    size_t len = strlen(s);
    if (len > 30) len = 30;
    for (size_t i=0; i<len; i++) str[1+i] = s[i];
    str[0] = (TUSB_DESC_STRING << 8) | (2*len + 2);
    return str;
}

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    if (instance == 0) return desc_mouse_report;
    else return desc_keyboard_report;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t type, const uint8_t *buffer, uint16_t bufsize) {}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t type, uint8_t *buffer, uint16_t reqlen) { return 0; }

//--------------------------------------------------------------------+
// Send Functions
//--------------------------------------------------------------------+
static void send_mouse(int8_t dx, int8_t dy) {
    uint8_t report[5] = {0, dx, dy, 0, 0};
    tud_hid_report(1, report, 5);
}

static void send_keyboard(uint8_t modifier, uint8_t keycode) {
    uint8_t report[8] = {modifier, 0x00, keycode, 0, 0, 0, 0, 0};
    tud_hid_report(1, report, 8);
}

// 发送空键盘报告（释放所有按键）
static void send_keyboard_release(void) {
    uint8_t report[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    tud_hid_report(1, report, 8);
}

//--------------------------------------------------------------------+
// Main
//--------------------------------------------------------------------+
int main(void) {
    board_init();
    tusb_init();

    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, 1);

    int step = 0;
    uint32_t last_key_time = 0;
    bool key_pressed = false;

    while (1) {
        tud_task();

        uint32_t now = to_ms_since_boot(get_absolute_time());

        // 闪烁 LED
        static uint32_t last_led = 0;
        if (now - last_led > 500) {
            gpio_put(25, !gpio_get(25));
            last_led = now;
        }

        // 只有在 HID 就绪时才发送
        if (tud_hid_ready()) {
            // --- 鼠标：画圈（降低频率，每50ms一次）---
            static uint32_t last_mouse = 0;
            if (now - last_mouse > 50) {
                int8_t dx = 3 * (step % 20 - 10) / 10;
                int8_t dy = 3 * (step % 15 - 7) / 10;
                send_mouse(dx, dy);
                step++;
                last_mouse = now;
            }

            // --- 键盘：每5秒发送空格 ---
            if (now - last_key_time > 5000) {
                // 发送按下空格
                send_keyboard(0, HID_KEY_SPACE);
                sleep_ms(20);   // 等待 Mac 处理
                // 发送释放报告（重要！必须释放）
                send_keyboard_release();
                last_key_time = now;
                // 额外延迟确保键盘动作完成
                sleep_ms(50);
            }
        }
    }
}
