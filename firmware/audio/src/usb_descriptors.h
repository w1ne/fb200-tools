/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#pragma once

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
    STRID_AUDIO,
    STRID_HID,
};

/* Interface layout: CDC first (keeps the console tty path stable), then the
 * UAC2 audio function (IAD covering AC + two AS interfaces), then the vendor
 * HID interface the stock editor and `fb200` talk to. */
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_AUDIO_CONTROL,
    ITF_NUM_AUDIO_STREAMING_SPK,
    ITF_NUM_AUDIO_STREAMING_MIC,
    ITF_NUM_HID,          /* vendor HID: the stock app protocol (docs/PROTOCOL.md) */
    ITF_NUM_TOTAL,
};

/* UAC2 entity IDs (arbitrary but unique). */
#define UAC2_ENTITY_CLOCK               0x04
#define UAC2_ENTITY_SPK_INPUT_TERMINAL  0x01
#define UAC2_ENTITY_SPK_FEATURE_UNIT    0x02
#define UAC2_ENTITY_SPK_OUTPUT_TERMINAL 0x03
#define UAC2_ENTITY_MIC_INPUT_TERMINAL  0x11
#define UAC2_ENTITY_MIC_OUTPUT_TERMINAL 0x13

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_AUDIO_OUT 0x03
#define EPNUM_AUDIO_IN  0x83
#define EPNUM_AUDIO_INT 0x84
#define EPNUM_HID_OUT   0x05
#define EPNUM_HID_IN    0x85

/* UAC2 function: stereo playback (host -> pedal) + stereo capture
 * (pedal -> host), 44.1 kHz, 16-bit, one format, no feedback endpoint
 * (capture is declared adaptive; the engine absorbs drift). */
#define TUD_AUDIO20_FB200_DESC_LEN (TUD_AUDIO20_DESC_IAD_LEN \
    + TUD_AUDIO20_DESC_STD_AC_LEN \
    + TUD_AUDIO20_DESC_CS_AC_LEN \
    + TUD_AUDIO20_DESC_CLK_SRC_LEN \
    + TUD_AUDIO20_DESC_INPUT_TERM_LEN \
    + TUD_AUDIO20_DESC_FEATURE_UNIT_LEN(CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX) \
    + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN \
    + TUD_AUDIO20_DESC_INPUT_TERM_LEN \
    + TUD_AUDIO20_DESC_OUTPUT_TERM_LEN \
    + TUD_AUDIO20_DESC_STD_AC_INT_EP_LEN \
    + TUD_AUDIO20_DESC_STD_AS_LEN \
    + TUD_AUDIO20_DESC_STD_AS_LEN \
    + TUD_AUDIO20_DESC_CS_AS_INT_LEN \
    + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN \
    + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN \
    + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN \
    + TUD_AUDIO20_DESC_STD_AS_LEN \
    + TUD_AUDIO20_DESC_STD_AS_LEN \
    + TUD_AUDIO20_DESC_CS_AS_INT_LEN \
    + TUD_AUDIO20_DESC_TYPE_I_FORMAT_LEN \
    + TUD_AUDIO20_DESC_STD_AS_ISO_EP_LEN \
    + TUD_AUDIO20_DESC_CS_AS_ISO_EP_LEN)
