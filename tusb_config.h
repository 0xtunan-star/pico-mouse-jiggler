#pragma once

#ifdef __cplusplus
extern "C" {
#endif


#define CFG_TUSB_MCU                OPT_MCU_RP2040


#define CFG_TUD_ENABLED             1


#define CFG_TUD_ENDPOINT0_SIZE      64



#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 0

// 两个 HID Interface
#define CFG_TUD_HID                 2



#define CFG_TUD_HID_EP_BUFSIZE      16



#define CFG_TUSB_RHPORT0_MODE \
        (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)



#ifdef __cplusplus
}
#endif
