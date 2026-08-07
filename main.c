#include "pico/stdlib.h"
#include "tusb.h"


#define HID_MOUSE_INSTANCE      0
#define HID_KEYBOARD_INSTANCE   1


static void send_mouse(int8_t x, int8_t y)
{
    uint8_t report[5] =
    {
        0,
        x,
        y,
        0,
        0
    };


    tud_hid_report(
        HID_MOUSE_INSTANCE,
        0,
        report,
        sizeof(report)
    );
}



static void send_keyboard(uint8_t key)
{
    uint8_t report[8] =
    {
        0,
        0,
        key,
        0,
        0,
        0,
        0,
        0
    };


    tud_hid_report(
        HID_KEYBOARD_INSTANCE,
        0,
        report,
        sizeof(report)
    );
}



int main()
{

    board_init();

    tusb_init();


    gpio_init(25);

    gpio_set_dir(
        25,
        GPIO_OUT
    );


    uint32_t last_key = 0;

    int step = 0;



    while(1)
    {

        tud_task();



        uint32_t now =
        to_ms_since_boot(
            get_absolute_time()
        );



        static uint32_t led_time = 0;


        if(now-led_time > 500)
        {

            gpio_put(
                25,
                !gpio_get(25)
            );


            led_time = now;
        }





        if(tud_mounted())
        {


            /*
                Mouse move
            */


            int8_t dx =
                (step % 20) - 10;


            int8_t dy =
                (step % 15) - 7;



            send_mouse(
                dx / 3,
                dy / 3
            );


            step++;





            /*
                Keyboard space every 5 seconds
            */


            if(now-last_key > 5000)
            {


                // press space

                send_keyboard(
                    HID_KEY_SPACE
                );


                sleep_ms(100);



                // release

                send_keyboard(0);



                last_key = now;

            }

        }



        sleep_ms(20);

    }


}
