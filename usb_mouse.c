#include "pico/stdlib.h"
#include "tusb.h"
#include <string.h>


#define REPORT_ID_MOUSE     1
#define REPORT_ID_KEYBOARD  2



// ===============================
// HID Report Descriptor
// ===============================

uint8_t const desc_hid_report[] =
{
    // Mouse
    TUD_HID_REPORT_DESC_MOUSE(
        HID_REPORT_ID(REPORT_ID_MOUSE)
    ),


    // Keyboard
    TUD_HID_REPORT_DESC_KEYBOARD(
        HID_REPORT_ID(REPORT_ID_KEYBOARD)
    )
};



// ===============================
// Device Descriptor
// ===============================


tusb_desc_device_t const desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,

    .bcdUSB             = 0x0200,

    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,

    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,


    // Microsoft OS 保留 VID
    .idVendor           = 0xCafe,
    .idProduct          = 0x4010,

    .bcdDevice          = 0x0100,


    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 3,

    .bNumConfigurations = 1
};





// ===============================
// Configuration Descriptor
// ===============================


#define CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)



uint8_t const desc_configuration[] =
{

    TUD_CONFIG_DESCRIPTOR(
        1,
        1,
        0,
        CONFIG_TOTAL_LEN,

        TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,

        100
    ),


    TUD_HID_DESCRIPTOR(
        0,
        0,

        HID_ITF_PROTOCOL_NONE,

        sizeof(desc_hid_report),

        0x81,

        64,

        5
    )

};




// ===============================
// String Descriptor
// ===============================


uint16_t const *tud_descriptor_string_cb(
    uint8_t index,
    uint16_t langid
)
{
    static uint16_t buffer[32];

    (void)langid;


    if(index==0)
    {
        buffer[0]=
            (TUSB_DESC_STRING<<8) | 4;

        buffer[1]=0x0409;

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


    uint8_t len=strlen(str);


    buffer[0]=
        (TUSB_DESC_STRING<<8)
        |
        (2*len+2);


    for(int i=0;i<len;i++)
    {
        buffer[i+1]=str[i];
    }


    return buffer;
}





const uint8_t *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}



const uint8_t *tud_descriptor_configuration_cb(
    uint8_t index
)
{
    (void)index;

    return desc_configuration;
}



const uint8_t *tud_hid_descriptor_report_cb(
    uint8_t instance
)
{
    (void)instance;

    return desc_hid_report;
}




void tud_hid_set_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t type,
    uint8_t const *buffer,
    uint16_t bufsize
)
{

}



uint16_t tud_hid_get_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t type,
    uint8_t *buffer,
    uint16_t reqlen
)
{
    return 0;
}





// ===============================
// HID Send
// ===============================


void send_mouse(
    int8_t x,
    int8_t y
)
{

    uint8_t report[5];


    report[0]=0;

    report[1]=x;

    report[2]=y;

    report[3]=0;

    report[4]=0;



    tud_hid_report(
        REPORT_ID_MOUSE,
        report,
        5
    );
}




void send_keyboard(
    uint8_t modifier,
    uint8_t key
)
{

    uint8_t report[8]={0};


    report[0]=modifier;

    report[2]=key;


    tud_hid_report(
        REPORT_ID_KEYBOARD,
        report,
        8
    );

}







// ===============================
// MAIN
// ===============================


int main()
{

    board_init();


    tusb_init();


    gpio_init(25);

    gpio_set_dir(
        25,
        GPIO_OUT
    );



    uint32_t last_key=0;

    int step=0;


    while(1)
    {

        tud_task();



        uint32_t now=
            to_ms_since_boot(
                get_absolute_time()
            );



        static uint32_t led=0;


        if(now-led>500)
        {
            gpio_put(
                25,
                !gpio_get(25)
            );

            led=now;
        }




        if(tud_hid_ready())
        {


            // -----------------
            // Mouse movement
            // -----------------

            int8_t dx =
                (step%20)-10;


            int8_t dy =
                (step%15)-7;



            send_mouse(
                dx/3,
                dy/3
            );


            step++;



            // -----------------
            // Keyboard
            // -----------------

            if(now-last_key>5000)
            {


                // press space

                send_keyboard(
                    0,
                    HID_KEY_SPACE
                );


                sleep_ms(100);



                // release

                send_keyboard(
                    0,
                    0
                );



                last_key=now;

            }



        }


        sleep_ms(20);

    }


}
