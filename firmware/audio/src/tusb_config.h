/* TinyUSB configuration for the FB200 (i.MX RT10xx, device-only, no OS). */
#pragma once

#define CFG_TUSB_MCU            OPT_MCU_MIMXRT10XX
#define CFG_TUSB_OS             OPT_OS_NONE
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED
#define CFG_TUD_CDC             1
#define CFG_TUD_CDC_RX_BUFSIZE  64
#define CFG_TUD_CDC_TX_BUFSIZE  64
#define CFG_TUD_CDC_EP_BUFSIZE  64
#define CFG_TUSB_MEM_ALIGN      __attribute__((aligned(4)))
