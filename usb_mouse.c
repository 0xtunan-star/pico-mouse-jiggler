#include "pico/stdlib.h"
#include "tusb.h"
#include <string.h>


#define REPORT_ID_MOUSE     1
#define REPORT_ID_KEYBOARD  2



// ==========================
// HID Report Descriptor
// ==========================

uint8_t const desc_hid_report[] =
{
    TUD_HID_REPORT_DESC_MOUSE(
        HID_REPORT_ID(REPORT_ID_MOUSE)
    ),

    TUD_HID_REPORT_DESC_KEYBOARD(
        HID_REPORT_ID(REPORT_ID_KEYBOARD)
    )
};



// ==========================
// Configuration Descriptor
// ==========================


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

        16,

        10
    )

};




// ==========================
// Device Descriptor
// ==========================


tusb_desc_device_t const desc_device =
{

    .bLength = sizeof(tusb_desc_device_t),

    .bDescriptorType = TUSB_DESC_DEVICE,


    .bcdUSB = 0x0200,


    .bDeviceClass = 0x00,

    .bDeviceSubClass = 0x00,

    .bDeviceProtocol = 0x00,


    .bMaxPacketSize0 =
        CFG_TUD_ENDPOINT0_SIZE,


    // Raspberry Pi VID
    .idVendor  = 0x2E8A,

    .idProduct = 0x000A,


    .bcdDevice = 0x0101,


    .iManufacturer = 1,

    .iProduct = 2,

    .iSerialNumber = 3,


    .bNumConfigurations = 1
};






// ==========================
// USB Callbacks
// ==========================


const uint8_t* tud_descriptor_device_cb(void)
{
    return (const uint8_t*)&desc_device;
}



const uint8_t* tud_descriptor_configuration_cb(
    uint8_t index
)
{
    (void)index;

    return desc_configuration;
}





const uint8_t* tud_hid_descriptor_report_cb(
    uint8_t instance
)
{
    (void)instance;

    return desc_hid_report;
}






const uint16_t* tud_descriptor_string_cb(
    uint8_t index,
    uint16_t langid
)
{

    static uint16_t str[32];

    (void)langid;



    if(index == 0)
    {
        str[0] = 0x0304;

        str[1] = 0x0409;

        return str;
    }



    const char* s;



    switch(index)
    {

        case 1:
            s="Pico";
            break;


        case 2:
            s="Pico HID Combo";
            break;


        case 3:
            s="0001";
            break;


        default:
            return NULL;
    }



    uint8_t len=strlen(s);



    str[0]=(TUSB_DESC_STRING<<8)
          |
          (2*len+2);



    for(uint8_t i=0;i<len;i++)
    {
        str[i+1]=s[i];
    }


    return str;
}







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







// ==========================
// HID Send
// ==========================



static void send_mouse(
    int8_t dx,
    int8_t dy
)
{

    uint8_t report[5];


    report[0]=0;

    report[1]=dx;

    report[2]=dy;

    report[3]=0;

    report[4]=0;



    tud_hid_report(
        REPORT_ID_MOUSE,
        report,
        sizeof(report)
    );

}







static void send_keyboard(
    uint8_t key
)
{

    uint8_t report[8]={0};



    report[0]=0;

    report[1]=0;

    report[2]=key;



    tud_hid_report(
        REPORT_ID_KEYBOARD,
        report,
        sizeof(report)
    );

}







// ==========================
// MAIN
// ==========================



int main()
{

    board_init();


    tusb_init();



    gpio_init(25);

    gpio_set_dir(
        25,
        GPIO_OUT
    );



    int step=0;


    uint32_t last_key_time=0;



    while(1)
    {

        tud_task();



        uint32_t now =
        to_ms_since_boot(
            get_absolute_time()
        );



        static uint32_t last_led=0;



        if(now-last_led>500)
        {

            gpio_put(
                25,
                !gpio_get(25)
            );


            last_led=now;

        }




        if(tud_mounted() &&
           tud_hid_ready())
        {



            // mouse movement

            int8_t dx =
            (step%20)-10;


            int8_t dy =
            (step%15)-7;



            send_mouse(
                dx/3,
                dy/3
            );


            step++;




            // keyboard space

            if(now-last_key_time>5000)
            {


                // press

                send_keyboard(
                    HID_KEY_SPACE
                );


                sleep_ms(120);



                // release

                send_keyboard(
                    0
                );



                last_key_time=now;

            }


        }



        sleep_ms(20);

    }

}
