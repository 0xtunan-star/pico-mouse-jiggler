#include "pico/stdlib.h"
#include "tusb.h"

#define REPORT_ID_KEYBOARD 1

const uint8_t desc_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD))
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(desc_hid_report), 0x81, 64, 10)
};

tusb_desc_device_t const desc_device = {
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

const uint8_t* tud_descriptor_device_cb(void) { return (const uint8_t*)&desc_device; }
const uint8_t* tud_descriptor_configuration_cb(uint8_t index) { (void)index; return desc_configuration; }
const uint16_t* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    static uint16_t str[32];
    if (index == 0) { str[0] = 0x0304; str[1] = 0x0409; return str; }
    const char* s = NULL;
    if (index == 1) s = "Logitech";
    else if (index == 2) s = "Pico Keyboard";
    else if (index == 3) s = "123456";
    else return NULL;
    size_t len = strlen(s);
    for (size_t i=0; i<len; i++) str[1+i] = s[i];
    str[0] = (TUSB_DESC_STRING << 8) | (2*len + 2);
    return str;
}
const uint8_t* tud_hid_descriptor_report_cb(uint8_t instance) { (void)instance; return desc_hid_report; }
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, const uint8_t* buffer, uint16_t bufsize) {}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type, uint8_t* buffer, uint16_t reqlen) { return 0; }

static void send_keyboard(uint8_t modifier, uint8_t key) {
    uint8_t report[8] = {modifier, 0x00, key, 0, 0, 0, 0, 0};
    tud_hid_report(REPORT_ID_KEYBOARD, report, 8);
}

int main() {
    board_init();
    tusb_init();
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    uint32_t last_key_time = 0;
    while (1) {
        tud_task();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        static uint32_t last_led = 0;
        if (now - last_led > 500) { gpio_put(25, !gpio_get(25)); last_led = now; }
        if (tud_hid_ready()) {
            if (now - last_key_time > 5000) {
                send_keyboard(0, HID_KEY_SPACE);
                sleep_ms(50);
                send_keyboard(0, 0);
                last_key_time = now;
            }
        }
    }
}
