#ifndef FB200_PROTO_H
#define FB200_PROTO_H
/* FB200 app protocol (docs/PROTOCOL.md): the AA 55 frame format that the
 * stock firmware speaks to the official Electron editor (USB HID) and the
 * phone app (BLE through the Bluetooth module's UART). Transport independent:
 * a transport pushes the raw frame bytes it receives into proto_feed() and
 * registers a sender that transmits whole frames (the sender does its own
 * chunking: 63-byte HID reports with a length byte, or 70-byte UART chunks
 * for BLE as the stock does).
 *
 * Frame: AA 55 | len u16 LE (= 1 + payload) | fn | payload | CRC16 BE over
 * len..payload (table CRC-CCITT, final XOR 0xFFFF). No identify byte: the
 * stock ignores frames whose byte 4 is not a command, so do we. */
#include <stdbool.h>
#include <stdint.h>

#define PROTO_MAX_LEN 0x3FAu                    /* stock rejects len > 0x3FA */
#define PROTO_MAX_FRAME (PROTO_MAX_LEN + 6u)

/* Transport ids are also the reply masks, as in the stock (1 = USB, 2 = BLE). */
typedef enum { PROTO_USB = 1, PROTO_BLE = 2 } proto_transport_t;
#define PROTO_ALL (PROTO_USB | PROTO_BLE)

/* Version strings reported by fn 0x00 / 0xFA / 0xC3 (7-byte fields). */
#ifndef PROTO_PRODUCT
#define PROTO_PRODUCT "FB200"
#endif
#ifndef PROTO_APP_VERSION
#define PROTO_APP_VERSION "V1.0.0"
#endif
#ifndef PROTO_FW_VERSION
#define PROTO_FW_VERSION "V2.0.0"
#endif
#ifndef PROTO_BT_VERSION
#define PROTO_BT_VERSION "V1.0.0"
#endif
#ifndef PROTO_HW_REV
#define PROTO_HW_REV "A"
#endif

typedef void (*proto_send_fn)(const uint8_t *frame, uint32_t len);

void proto_init(void);                                   /* loads the IR/aux state from flash */
void proto_set_sender(proto_transport_t t, proto_send_fn fn);
void proto_feed(proto_transport_t t, const uint8_t *data, uint32_t n);
/* Unsolicited notifications go to this mask (stock: BLE only). */
void proto_set_notify_mask(uint8_t mask);

/* Build one frame into out (cap >= payload + 7). Returns the frame length. */
uint32_t proto_encode(uint8_t fn, const uint8_t *payload, uint32_t n, uint8_t *out, uint32_t cap);
uint16_t proto_crc16(const uint8_t *d, uint32_t n);

/* Notifications for the UI (what the stock sends when the front panel
 * changes something). module: 0..6 = fn 0x80..0x86. */
void proto_notify_module(unsigned module);
void proto_notify_preset(void);          /* 0x98 [index] (footswitch/bank change) */
void proto_notify_settings(void);        /* 0xB0 settings block (tuner toggle etc.) */
void proto_notify_rhythm(void);          /* 0xBA rhythm block */
void proto_notify_rhythm_mode(void);     /* 0xC9 [settings+0x20] */
void proto_notify_battery(void);         /* 0xBB [percent, charging] */
void proto_notify_saved(void);           /* 0x97 [index][preset] + 0x98 (long-A save) */

/* Rhythm settings block (F:0x81000, 6 bytes: on, ?, pattern 0..39, volume,
 * tempo u16 LE; default 0,0,0,100,110). */
const uint8_t *proto_rhythm(void);

/* ---- platform hooks ---------------------------------------------------
 * Storage (strong, provided by proto_port.c on the target, by the test on
 * the host). Offsets are flash offsets from 0x60000000. write returns 0 on
 * success and may span sectors. */
void proto_flash_read(uint32_t off, void *dst, uint32_t n);
int proto_flash_write(uint32_t off, const void *src, uint32_t n);
void proto_battery(uint8_t *percent, uint8_t *charging);

/* Actions (weak defaults in proto.c do nothing): */
void proto_hook_bt_name(const uint8_t name[20]);  /* 0xB3; stock: AT+BD<name+5> Audio, AT+BM<name> */
void proto_hook_bt_enable(bool on);               /* settings+0x17 changed; stock: AT+B501/B500, AT+CZ */
void proto_hook_bootloader(void);                 /* 0xC1/0xC4: flag F:0x86000 = 0 already written */
int proto_hook_factory_reset(void);               /* 0xB2: presets + settings to factory, 0 = ok */
void proto_hook_ir_changed(unsigned slot);        /* 0..8: user IR stored/deleted/renamed */

/* IR store layout (stock): names 9 x 50 at F:0x87000, used flags at
 * F:0x88000, data 0x2800 bytes per slot at F:0x89000 + slot * 0x2800. */
#define IR_SLOTS 9u
#define IR_NAME_LEN 50u
#define IR_NAMES_FLASH 0x00087000u
#define IR_FLAGS_FLASH 0x00088000u
#define IR_DATA_FLASH 0x00089000u
#define IR_DATA_SIZE 0x2800u
#define RHYTHM_FLASH 0x00081000u
#define BTNAME_FLASH 0x00083000u
#define UPDATE_FLAG_FLASH 0x00086000u
#define AUX_FLASH 0x00085000u              /* 30-byte block of fn 0xD9/0xDA */
#endif
