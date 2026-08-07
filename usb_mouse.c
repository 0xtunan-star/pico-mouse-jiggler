#include <stdio.h>
#include "pico/stdlib.h"
#include "tusb.h"

#define REPORT_ID_MOUSE 1

const uint8_t desc_hid_report[] = {
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE))
};

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN, 0, 100),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(desc_hid_report), 0x81, 64, 10)
};

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
const uint8_t* tud_hid_descriptor_report_cb(uint8_t itf) { (void)itf; return desc_hid_report; }
uint8_t const* tud_descriptor_configuration_cb(uint8_t idx) { (void)idx; return desc_configuration; }

static const char* string_desc_arr[] = {
    "\x09\x04", "Logitech", "Pure Mouse", "123456"
};
const uint16_t* tud_descriptor_string_cb(uint8_t idx, uint16_t lang) {
    (void)lang;
    static uint16_t str[20];
    if (idx == 0) { memcpy(&str[1], string_desc_arr[0], 2); str[0] = 0x0304; return str; }
    if (idx >= sizeof(string_desc_arr)/sizeof(char*)) return NULL;
    const char* s = string_desc_arr[idx];
    uint8_t len = strlen(s);
    if (len > 18) len = 18;
    for (int i=0; i<len; i++) str[1+i] = s[i];
    str[0] = (TUSB_DESC_STRING << 8) | (2*len + 2);
    return str;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t id, hid_report_type_t type, const uint8_t* buf, uint16_t size) {}
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t id, hid_report_type_t type, uint8_t* buf, uint16_t req) { return 0; }

int main() {
    board_init();
    tusb_init();
    int cnt = 0;
    while (1) {
        tud_task();
        if (tud_hid_ready()) {
            int8_t dx = (cnt % 20) - 10;
            int8_t dy = (cnt % 15) - 7;
            tud_hid_report(REPORT_ID_MOUSE, (uint8_t[]){ 0, dx, dy, 0, 0 }, 5);
            cnt++;
            sleep_ms(50);
        }
    }
}
