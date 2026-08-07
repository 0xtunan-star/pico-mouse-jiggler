/* 
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "tusb.h"

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+

// HID report descriptor using TinyUSB's template
// Single Report ID 1 for both mouse and keyboard
// We'll use separate interfaces, so each has its own descriptor

// Mouse Report Descriptor (ID 1)
static const uint8_t desc_mouse_report[] = {
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(1))
};

// Keyboard Report Descriptor (ID 1)
static const uint8_t desc_keyboard_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1))
};

//--------------------------------------------------------------------+
// Configuration Descriptor
//--------------------------------------------------------------------+
enum {
    ITF_NUM_MOUSE = 0,
    ITF_NUM_KEYBOARD,
    ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN * 2)
#define EPNUM_MOUSE   0x81
#define EPNUM_KEYBOARD 0x82

static const uint8_t desc_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface 0: Mouse
    TUD_HID_DESCRIPTOR(ITF_NUM_MOUSE, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(desc_mouse_report), EPNUM_MOUSE, CFG_TUD_HID_EP_BUFSIZE, 10),

    // Interface 1: Keyboard
    TUD_HID_DESCRIPTOR(ITF_NUM_KEYBOARD, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(desc_keyboard_report), EPNUM_KEYBOARD, CFG_TUD_HID_EP_BUFSIZE, 10),
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
    .idVendor = 0x046D, // Logitech
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
    (const char[]){0x09, 0x04}, // 0: English
    "Logitech",
    "Pico Combo",
    "123456"
};

//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+
const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t str[32];
    if (index == 0) {
        memcpy(&str[1], string_desc_arr[0], 2);
        str[0] = 0x0304;
        return str;
    }
    if (index >= sizeof(string_desc_arr)/sizeof(string_desc_arr[0])) return NULL;
    const char *s = string_desc_arr[index];
    size_t len = strlen(s);
    if (len > 30) len = 30;
    for (size_t i=0; i<len; i++) str[1+i] = s[i];
    str[0] = (TUSB_DESC_STRING << 8) | (2*len + 2);
    return str;
}

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    if (instance == ITF_NUM_MOUSE) return desc_mouse_report;
    else return desc_keyboard_report;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           const uint8_t *buffer, uint16_t bufsize) {
    // Not used for this example
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    return 0;
}

//--------------------------------------------------------------------+
// Application Functions
//--------------------------------------------------------------------+
static void send_mouse_report(int8_t x, int8_t y) {
    uint8_t report[5] = {0, x, y, 0, 0}; // buttons, x, y, wheel, pan
    tud_hid_report(1, report, 5);
}

static void send_keyboard_report(uint8_t modifier, uint8_t keycode) {
    uint8_t report[8] = {modifier, 0x00, keycode, 0, 0, 0, 0, 0};
    tud_hid_report(1, report, 8);
}

static void send_keyboard_release(void) {
    uint8_t report[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    tud_hid_report(1, report, 8);
}

int main(void) {
    board_init();
    tusb_init();

    // Initialize LED
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, 1);

    int step = 0;
    uint32_t last_key_time = 0;

    while (true) {
        tud_task(); // tinyusb device task

        uint32_t now = to_ms_since_boot(get_absolute_time());

        // Blink LED to indicate running
        static uint32_t last_led = 0;
        if (now - last_led > 500) {
            gpio_put(25, !gpio_get(25));
            last_led = now;
        }

        if (tud_hid_ready()) {
            // ---- Mouse: draw small circle ----
            // Move every 20ms for smooth animation
            static uint32_t last_mouse = 0;
            if (now - last_mouse > 20) {
                int8_t dx = 3 * (step % 20 - 10) / 10;
                int8_t dy = 3 * (step % 15 - 7) / 10;
                send_mouse_report(dx, dy);
                step++;
                last_mouse = now;
            }

            // ---- Keyboard: send space every 5 seconds ----
            if (now - last_key_time > 5000) {
                send_keyboard_report(0, HID_KEY_SPACE);
                sleep_ms(20);
                send_keyboard_release();
                last_key_time = now;
                // Small delay to let keyboard report go through
                sleep_ms(20);
            }
        }
    }
}
