// 官方 hid_composite 示例简化版（鼠标画圆 + 键盘发送字母）
#include <stdio.h>
#include "pico/stdlib.h"
#include "tusb.h"

//--------------------------------------------------------------------+
// 报告描述符
//--------------------------------------------------------------------+
uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(1)),
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(2))
};

//--------------------------------------------------------------------+
// 设备/配置/字符串描述符
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x046d,
    .idProduct = 0xc539,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

const uint8_t* tud_descriptor_device_cb(void) { return (const uint8_t*)&desc_device; }

const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    static const char* str[] = { "\x09\x04", "Logitech", "Composite", "123" };
    static uint16_t buf[32];
    if (index == 0) { buf[0] = 0x0304; memcpy(&buf[1], str[0], 2); return buf; }
    if (index >= 4) return NULL;
    const char* s = str[index];
    size_t len = strlen(s);
    for (size_t i=0; i<len; i++) buf[1+i] = s[i];
    buf[0] = (TUSB_DESC_STRING << 8) | (2*len + 2);
    return buf;
}

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + 2*TUD_HID_DESC_LEN, 0, 100),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(desc_hid_report), 0x81, 64, 10),
    TUD_HID_DESCRIPTOR(1, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(desc_hid_report), 0x82, 64, 10)
};

const uint8_t* tud_descriptor_configuration_cb(uint8_t idx) { (void)idx; return desc_configuration; }
const uint8_t* tud_hid_descriptor_report_cb(uint8_t itf) { (void)itf; return desc_hid_report; }

// HID 回调
void tud_hid_set_report_cb(uint8_t itf, uint8_t id, hid_report_type_t type, const uint8_t* buf, uint16_t size) {}
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t id, hid_report_type_t type, uint8_t* buf, uint16_t req) { return 0; }

//--------------------------------------------------------------------+
// 主程序
//--------------------------------------------------------------------+
int main() {
    board_init();
    tusb_init();

    int cnt = 0;
    uint32_t last_key = 0;

    while (1) {
        tud_task();

        if (tud_hid_ready()) {
            // 鼠标：画圆
            int8_t dx = 5 * (cnt % 20 - 10) / 10;
            int8_t dy = 5 * (cnt % 15 - 7) / 10;
            uint8_t report[5] = { 0, dx, dy, 0, 0 };
            tud_hid_report(1, report, 5);
            cnt++;

            // 键盘：每 5 秒发送一次 'a'
            uint32_t now = to_ms_since_boot(get_absolute_time());
            if (now - last_key > 5000) {
                uint8_t kbd[8] = { 0, 0, HID_KEY_A, 0, 0, 0, 0, 0 };
                tud_hid_report(2, kbd, 8);
                sleep_ms(50);
                kbd[2] = 0;
                tud_hid_report(2, kbd, 8);
                last_key = now;
            }
        }
        sleep_ms(20);
    }
}
