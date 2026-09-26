#include "tusb.h"
#include "usb_descriptors.h"
#include "board_config.h"

static const tusb_desc_device_t desc_device TU_ATTR_ALIGNED(4) = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = HELLO_USB_VID,
    .idProduct = HELLO_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82

static const uint8_t desc_configuration[] TU_ATTR_ALIGNED(4) = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(0, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static const char *const strings[] = {
        [STRID_MANUFACTURER] = HELLO_USB_MANUFACTURER,
        [STRID_PRODUCT] = HELLO_USB_PRODUCT,
        [STRID_SERIAL] = HELLO_USB_SERIAL,
        [STRID_CDC] = "FB200 Hello CDC",
    };
    uint8_t chr_count;
    if (index == STRID_LANGID) {
        _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 + 2);
        _desc_str[1] = 0x0409;
        chr_count = 1;
    } else if (index < sizeof(strings) / sizeof(strings[0]) && strings[index]) {
        const char *str = strings[index];
        chr_count = 0;
        while (str[chr_count] && chr_count < 31) {
            _desc_str[1 + chr_count] = (uint16_t)str[chr_count];
            chr_count++;
        }
        _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    } else {
        return NULL;
    }
    return _desc_str;
}
