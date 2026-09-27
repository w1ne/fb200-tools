#include "tusb.h"
#include "usb_descriptors.h"
#include "audio/audio_config.h"
_Static_assert(CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE == AUDIO_FS, "USB rate != AUDIO_FS");
#include "board_config.h"

static const tusb_desc_device_t desc_device TU_ATTR_ALIGNED(4) = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = AUDIO_USB_VID,
    .idProduct = AUDIO_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_AUDIO20_FB200_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

/* Vendor HID, 64-byte reports without report IDs, as the stock: the first
 * byte of a report is the payload length (docs/PROTOCOL.md §2). */
static const uint8_t desc_hid_report[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(CFG_TUD_HID_EP_BUFSIZE),
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return desc_hid_report;
}

#define UAC2_FU_CTRL \
    (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_MUTE_POS) | \
    (AUDIO20_CTRL_RW << AUDIO20_FEATURE_UNIT_CTRL_VOLUME_POS)

#define UAC2_AS_CTRL_LEN \
    (TUD_AUDIO20_DESC_CLK_SRC_LEN \
     + TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX) \
     + TUD_AUDIO20_DESC_INPUT_TERM_LEN + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN \
     + TUD_AUDIO20_DESC_INPUT_TERM_LEN + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN)

static const uint8_t desc_configuration[] TU_ATTR_ALIGNED(4) = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT,
                       EPNUM_CDC_IN, 64),

    /* ---- UAC2 audio function ---- */
    TUD_AUDIO20_DESC_IAD(ITF_NUM_AUDIO_CONTROL, 3, 0),
    TUD_AUDIO20_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0x01, STRID_AUDIO),
    TUD_AUDIO20_DESC_CS_AC(0x0200, AUDIO20_FUNC_OTHER, UAC2_AS_CTRL_LEN,
                           AUDIO20_CS_AS_INTERFACE_CTRL_LATENCY_POS),
    TUD_AUDIO20_DESC_CLK_SRC(UAC2_ENTITY_CLOCK, 3, 7, 0x00, 0x00),
    TUD_AUDIO20_DESC_INPUT_TERM(UAC2_ENTITY_SPK_INPUT_TERMINAL,
                                AUDIO_TERM_TYPE_USB_STREAMING,
                                UAC2_ENTITY_MIC_OUTPUT_TERMINAL,
                                UAC2_ENTITY_CLOCK,
                                CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX,
                                AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00, 0,
                                0x00),
    TUD_AUDIO20_DESC_FEATURE_UNIT(UAC2_ENTITY_SPK_FEATURE_UNIT,
                                  UAC2_ENTITY_SPK_INPUT_TERMINAL, 0x00,
                                  UAC2_FU_CTRL, UAC2_FU_CTRL, UAC2_FU_CTRL),
    TUD_AUDIO20_DESC_OUTPUT_TERM(UAC2_ENTITY_SPK_OUTPUT_TERMINAL,
                                 AUDIO_TERM_TYPE_OUT_HEADPHONES, 0x00,
                                 UAC2_ENTITY_SPK_FEATURE_UNIT,
                                 UAC2_ENTITY_CLOCK, 0x0000, 0x00),
    TUD_AUDIO20_DESC_INPUT_TERM(UAC2_ENTITY_MIC_INPUT_TERMINAL,
                                AUDIO_TERM_TYPE_IN_GENERIC_MIC, 0x00,
                                UAC2_ENTITY_CLOCK,
                                CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX,
                                AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00, 0,
                                0x00),
    TUD_AUDIO20_DESC_OUTPUT_TERM(UAC2_ENTITY_MIC_OUTPUT_TERMINAL,
                                 AUDIO_TERM_TYPE_USB_STREAMING,
                                 UAC2_ENTITY_SPK_INPUT_TERMINAL,
                                 UAC2_ENTITY_MIC_INPUT_TERMINAL,
                                 UAC2_ENTITY_CLOCK, 0x0000, 0x00),
    TUD_AUDIO20_DESC_STD_AC_INT_EP(EPNUM_AUDIO_INT, 0x01),

    /* Playback (host -> pedal): alt 0 (idle) + alt 1 (streaming) */
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_SPK, 0x00, 0x00, 0x00),
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_SPK, 0x01, 0x01, 0x00),
    TUD_AUDIO20_DESC_CS_AS_INT(UAC2_ENTITY_SPK_INPUT_TERMINAL, AUDIO20_CTRL_NONE,
                               AUDIO20_FORMAT_TYPE_I,
                               AUDIO20_DATA_FORMAT_TYPE_I_PCM,
                               CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX,
                               AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(
        CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX,
        CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX),
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(
        EPNUM_AUDIO_OUT,
        (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE |
                  TUSB_ISO_EP_ATT_DATA),
        CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX, 0x01),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(
        AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,
        AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),

    /* Capture (pedal -> host): alt 0 (idle) + alt 1 (streaming) */
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_MIC, 0x00, 0x00, 0x00),
    TUD_AUDIO20_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING_MIC, 0x01, 0x01, 0x00),
    TUD_AUDIO20_DESC_CS_AS_INT(UAC2_ENTITY_MIC_OUTPUT_TERMINAL,
                               AUDIO20_CTRL_NONE, AUDIO20_FORMAT_TYPE_I,
                               AUDIO20_DATA_FORMAT_TYPE_I_PCM,
                               CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX,
                               AUDIO20_CHANNEL_CONFIG_NON_PREDEFINED, 0x00),
    TUD_AUDIO20_DESC_TYPE_I_FORMAT(
        CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX,
        CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX),
    TUD_AUDIO20_DESC_STD_AS_ISO_EP(
        EPNUM_AUDIO_IN,
        (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE |
                  TUSB_ISO_EP_ATT_DATA),
        CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX, 0x01),
    TUD_AUDIO20_DESC_CS_AS_ISO_EP(
        AUDIO20_CS_AS_ISO_DATA_EP_ATT_NON_MAX_PACKETS_OK, AUDIO20_CTRL_NONE,
        AUDIO20_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_MILLISEC, 0x0001),

    /* ---- vendor HID (app protocol) ---- */
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, STRID_HID, HID_ITF_PROTOCOL_NONE,
                             sizeof(desc_hid_report), EPNUM_HID_OUT, EPNUM_HID_IN,
                             CFG_TUD_HID_EP_BUFSIZE, 1),
};

TU_VERIFY_STATIC(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
                 "config descriptor length mismatch");

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
        [STRID_MANUFACTURER] = AUDIO_USB_MANUFACTURER,
        [STRID_PRODUCT] = AUDIO_USB_PRODUCT,
        [STRID_SERIAL] = AUDIO_USB_SERIAL,
        [STRID_CDC] = "FB200 Audio CDC",
        [STRID_AUDIO] = "FB200 Audio I/O",
        [STRID_HID] = "FB200 Control",
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
