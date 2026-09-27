/* USB transport of the app protocol: the vendor HID interface (both images
 * declare it; recovery has no protocol engine and ignores its reports).
 * OUT reports: [len][len payload bytes][padding] -> proto_feed(PROTO_USB).
 * Frames to the host are split into 63-byte chunks, one report each,
 * queued and sent whenever the HID endpoint is free (docs/PROTOCOL.md §2). */
#include <string.h>
#include "tusb.h"
#ifndef FB200_RECOVERY
#include "proto/proto.h"
#endif

#ifndef FB200_RECOVERY
static uint8_t q[2048];
static uint32_t q_head, q_tail;

static void usb_send(const uint8_t *frame, uint32_t len)
{
    uint32_t used = (q_head - q_tail) % sizeof q;
    if (len >= sizeof q - used) return;           /* drop: host not reading */
    for (uint32_t i = 0; i < len; i++) { q[q_head] = frame[i]; q_head = (q_head + 1u) % sizeof q; }
}

void usb_hid_init(void) { proto_set_sender(PROTO_USB, usb_send); }
#endif

#ifndef FB200_RECOVERY
void usb_hid_task(void)
{
    if (q_head == q_tail || !tud_hid_ready()) return;
    uint8_t report[CFG_TUD_HID_EP_BUFSIZE] = {0};
    uint8_t n = 0;
    while (n < CFG_TUD_HID_EP_BUFSIZE - 1 && q_tail != q_head) {
        report[1 + n++] = q[q_tail];
        q_tail = (q_tail + 1u) % sizeof q;
    }
    report[0] = n;
    (void)tud_hid_report(0, report, sizeof report);
}
#endif

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                           uint8_t const *buf, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)type;
#ifdef FB200_RECOVERY
    (void)buf;
    (void)bufsize;
#else
    if (bufsize == 0) return;
    uint8_t n = buf[0];
    if (n > bufsize - 1u) n = (uint8_t)(bufsize - 1u);
    proto_feed(PROTO_USB, buf + 1, n);
#endif
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                               uint8_t *buf, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)type; (void)buf; (void)reqlen;
    return 0;
}
