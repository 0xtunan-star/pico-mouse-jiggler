/*
 * Copyright (c) 2021 Raspberry Pi (Trading) Ltd.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "tusb.h"

//--------------------------------------------------------------------+
// Device Descriptors
//--------------------------------------------------------------------+
#define USB_VID 0x046D
#define USB_PID 0xC539
#define USB_MANUFACTURER "Logitech"
#define USB_PRODUCT "Pico Combo"

// HID Report Descriptor
// Uses two report IDs: 1 for mouse, 2 for keyboard
static const uint8_t desc_hid_report[] = {
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(1)),
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(2))
};

// Configuration Descriptor: two interfaces (mouse + keyboard)
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + 2*TUD_HID_DESC_LEN)
static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(desc_hid_report), 0x81, CFG_TUD_HID_EP_BUFSIZE, 10),
    TUD_HID_DESCRIPTOR(1, 0, HID_ITF_PROTOCOL_KEYBOARD, sizeof(desc_hid_report), 0x82, CFG_TUD_HID_EP_BUFSIZE, 10)
};

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

// String descriptors
static const char *string_desc_arr[] = {
    (const char[]){0x09, 0x04}, // 0: English (0x0409)
    USB_MANUFACTURER,
    USB_PRODUCT,
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
    (void)instance;
    return desc_hid_report;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           const uint8_t *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

//--------------------------------------------------------------------+
// Application
//--------------------------------------------------------------------+
static void send_mouse_report(uint8_t buttons, int8_t dx, int8_t dy) {
    uint8_t report[5] = {buttons, dx, dy, 0, 0};
    tud_hid_report(1, report, 5);
}

static void send_keyboard_report(uint8_t modifier, uint8_t keycode) {
    uint8_t report[8] = {modifier, 0x00, keycode, 0, 0, 0, 0, 0};
    tud_hid_report(2, report, 8);
}

int main(void) {
    stdio_init_all();
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, 1);

    tusb_init();

    uint32_t last_key_time = 0;
    int step = 0;

    while (true) {
        tud_task();  // Process USB events

        uint32_t now = to_ms_since_boot(get_absolute_time());

        // Blink LED
        static uint32_t last_led = 0;
        if (now - last_led > 500) {
            gpio_put(25, !gpio_get(25));
            last_led = now;
        }

        // Mouse: draw a small circle
        if (tud_hid_ready()) {
            int8_t dx = 3 * (step % 20 - 10) / 10;
            int8_t dy = 3 * (step % 15 - 7) / 10;
            send_mouse_report(0, dx, dy);
            step++;
            sleep_ms(20);
        }

        // Keyboard: send space every 5 seconds
        if (now - last_key_time > 5000) {
            if (tud_hid_ready()) {
                send_keyboard_report(0, HID_KEY_SPACE);
                sleep_ms(50);
                send_keyboard_report(0, 0);
                last_key_time = now;
            }
        }
    }
}
