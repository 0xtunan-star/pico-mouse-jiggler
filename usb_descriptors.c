#include "tusb.h"
#include <string.h>


// =========================
// HID Interface
// =========================


enum
{
    ITF_MOUSE = 0,
    ITF_KEYBOARD,

    ITF_TOTAL
};



#define CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN*2)



// Mouse Report

uint8_t const desc_mouse_report[] =
{

    TUD_HID_REPORT_DESC_MOUSE()

};




// Keyboard Report

uint8_t const desc_keyboard_report[] =
{

    TUD_HID_REPORT_DESC_KEYBOARD()

};





// =========================
// Device Descriptor
// =========================


tusb_desc_device_t const desc_device =
{

    .bLength =
        sizeof(tusb_desc_device_t),


    .bDescriptorType =
        TUSB_DESC_DEVICE,


    .bcdUSB =
        0x0200,


    .bDeviceClass =
        0x00,


    .bDeviceSubClass =
        0x00,


    .bDeviceProtocol =
        0x00,


    .bMaxPacketSize0 =
        CFG_TUD_ENDPOINT0_SIZE,



    // Raspberry Pi VID

    .idVendor =
        0x2E8A,


    .idProduct =
        0x000A,



    .bcdDevice =
        0x0100,


    .iManufacturer =
        1,


    .iProduct =
        2,


    .iSerialNumber =
        3,


    .bNumConfigurations =
        1

};





// =========================
// Configuration Descriptor
// =========================



uint8_t const desc_configuration[] =
{


    TUD_CONFIG_DESCRIPTOR(
        1,
        ITF_TOTAL,
        0,
        CONFIG_TOTAL_LEN,
        0,
        100
    ),




    // Mouse HID


    TUD_HID_DESCRIPTOR(
        ITF_MOUSE,
        0,
        HID_ITF_PROTOCOL_MOUSE,

        sizeof(desc_mouse_report),

        0x81,

        16,

        10
    ),




    // Keyboard HID


    TUD_HID_DESCRIPTOR(
        ITF_KEYBOARD,
        0,
        HID_ITF_PROTOCOL_KEYBOARD,

        sizeof(desc_keyboard_report),

        0x82,

        16,

        10
    )


};






// =========================
// Callbacks
// =========================



uint8_t const* tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}




uint8_t const* tud_descriptor_configuration_cb(
    uint8_t index
)
{
    (void)index;

    return desc_configuration;
}






uint8_t const* tud_hid_descriptor_report_cb(
    uint8_t instance
)
{

    if(instance == ITF_MOUSE)
    {
        return desc_mouse_report;
    }


    if(instance == ITF_KEYBOARD)
    {
        return desc_keyboard_report;
    }


    return NULL;

}






// =========================
// String Descriptor
// =========================



uint16_t const* tud_descriptor_string_cb(
    uint8_t index,
    uint16_t langid
)
{

    static uint16_t buffer[32];


    (void)langid;



    if(index == 0)
    {

        buffer[0] = 0x0304;

        buffer[1] = 0x0409;

        return buffer;

    }



    const char *str;



    switch(index)
    {

        case 1:

            str="Pico";

            break;


        case 2:

            str="Pico HID Combo";

            break;


        case 3:

            str="0001";

            break;


        default:

            return NULL;

    }



    uint8_t len =
        strlen(str);



    buffer[0] =
        (TUSB_DESC_STRING<<8)
        |
        (2*len+2);



    for(uint8_t i=0;i<len;i++)
    {
        buffer[i+1]=str[i];
    }



    return buffer;

}




// =========================
// HID callbacks
// =========================


void tud_hid_set_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t type,
    uint8_t const* buffer,
    uint16_t bufsize
)
{

}




uint16_t tud_hid_get_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t type,
    uint8_t* buffer,
    uint16_t reqlen
)
{

    return 0;

}
