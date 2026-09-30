/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#include "ui/display.h"
#include "ui/pads.h"
#include "fsl_gpio.h"
#include "ui/power_logic.h"

/* GPIO4 segment bits (stock font at ITCM 0x16574). */
#define SA   (1u << 25)
#define SB   (1u << 24)
#define SC   (1u << 22)
#define SD   (1u << 21)
#define SE   (1u << 16)
#define SF   (1u << 30)
#define SG1  (1u << 26)
#define SG2  (1u << 20)
#define SMID ((1u << 18) | (1u << 28))   /* centre verticals */
#define SDIAG (1u << 17)                 /* the diagonal 'R' uses */
#define SDP  (1u << 23)
#define SEG_MASK 0x7FFF0000u             /* GPIO4_IO16..30 */
#define PAD_CFG 0x10B0u

static uint32_t glyph(char c)
{
    switch (c) {
    case '0': case 'O': return SA | SB | SC | SD | SE | SF;
    case 'o': return SC | SD | SE | SG1 | SG2;
    case 'H': case 'h': return SB | SC | SE | SF | SG1 | SG2;
    case 'N': case 'n': return SC | SE | SG1 | SG2;
    case 'r': return SE | SG1;
    case 'T': case 't': return SD | SE | SF | SG1 | SG2;
    case 'U': return SB | SC | SD | SE | SF;
    case 'u': return SC | SD | SE;
    case 'S': case 's': return SA | SC | SD | SF | SG1 | SG2;
    case 'Y': case 'y': return SB | SC | SD | SF | SG1 | SG2;
    case '1': return SB | SC;
    case '2': return SA | SB | SD | SE | SG1 | SG2;
    case '3': return SA | SB | SC | SD | SG1 | SG2;
    case '4': return SB | SC | SF | SG1 | SG2;
    case '5': return SA | SC | SD | SF | SG1 | SG2;
    case '6': return SA | SC | SD | SE | SF | SG1 | SG2;
    case '7': return SA | SB | SC;
    case '8': return SA | SB | SC | SD | SE | SF | SG1 | SG2;
    case '9': return SA | SB | SC | SD | SF | SG1 | SG2;
    case 'A': case 'a': return SA | SB | SC | SE | SF | SG1 | SG2;
    case 'B': case 'b': return SC | SD | SE | SF | SG1 | SG2;
    case 'C': case 'c': return SA | SD | SE | SF;
    case 'D': case 'd': return SB | SC | SD | SE | SG1 | SG2;
    case 'E': case 'e': return SA | SD | SE | SF | SG1 | SG2;
    case 'F': case 'f': return SA | SE | SF | SG1;
    case 'G': case 'g': return SA | SC | SD | SE | SF | SG2;
    case 'I': case 'i': return SA | SD | SMID;
    case 'L': case 'l': return SD | SE | SF;
    case 'P': case 'p': return SA | SB | SE | SF | SG1 | SG2;
    case 'R': return SA | SB | SE | SF | SG1 | SG2 | SDIAG;
    case '-': return SG1 | SG2;
    default: return 0;
    }
}

static uint32_t digits[3];
static unsigned cur, phase;
static uint32_t last_ms;
static uint16_t knob_mask;            /* bit per knob LED: on */
static unsigned duty = 3;             /* lit ms of each 3 ms digit slot */
/* override (ui/power.c): shown instead of the text, knob LEDs off */
static bool ovr;
static uint32_t ovr_digits[3];
static uint16_t ovr_on_ms, ovr_period_ms;

/* digit selects: d0 GPIO4_IO31, d1 GPIO3_IO18, d2 GPIO3_IO21 (active high) */
static void select_digit(int d)
{
    GPIO_PinWrite(GPIO4, 31u, d == 0);
    GPIO_PinWrite(GPIO3, 18u, d == 1);
    GPIO_PinWrite(GPIO3, 21u, d == 2);
}

static void to_digits(const char *s, uint32_t out[3])
{
    for (int d = 0; d < 3; d++) out[d] = 0;
    for (int d = 0; d < 3 && *s; s++) {
        if (*s == '.' && d > 0) { out[d - 1] |= SDP; continue; }
        out[d++] = glyph(*s);
        if (s[1] == '.') { out[d - 1] |= SDP; s++; }
    }
}

void display_init(void)
{
    gpio_pin_config_t out0 = {kGPIO_DigitalOutput, 0, kGPIO_NoIntmode};
    gpio_pin_config_t out1 = {kGPIO_DigitalOutput, 1, kGPIO_NoIntmode};
    for (uint32_t i = 0; i < 16; i++) {                 /* knob LEDs, active low: off */
        pad_set(PAD_EMC(i), 5u, PAD_CFG);
        GPIO_PinInit(GPIO4, i, &out1);
    }
    for (uint32_t i = 16; i <= 31; i++) {
        pad_set(PAD_EMC(i), 5u, PAD_CFG);
        GPIO_PinInit(GPIO4, i, &out0);
    }
    pad_set(PAD_EMC(32), 5u, PAD_CFG);
    GPIO_PinInit(GPIO3, 18u, &out0);
    pad_set(PAD_EMC(35), 5u, PAD_CFG);
    GPIO_PinInit(GPIO3, 21u, &out0);
    display_text("   ");
}

void display_text(const char *s) { to_digits(s, digits); }

void display_set_level(unsigned pct) { duty = power_duty_ms(pct); }

void display_override(const char *text, uint16_t on_ms, uint16_t period_ms)
{
    ovr = text != 0;
    if (ovr) to_digits(text, ovr_digits);
    ovr_on_ms = on_ms;
    ovr_period_ms = period_ms ? period_ms : 1u;
}

/* One step per ms: a digit keeps its 3 ms slot (as before); in the slot it
 * and the knob LEDs are lit for `duty` ms (brightness). */
void display_task(uint32_t now_ms)
{
    if (now_ms == last_ms) return;
    last_ms = now_ms;
    if (++phase >= 3u) {
        phase = 0;
        cur = (cur + 1u) % 3u;
    }
    bool lit = phase < duty;
    uint32_t seg = 0, leds = 0;
    if (ovr) {
        if (now_ms % ovr_period_ms < ovr_on_ms) seg = ovr_digits[cur];
    } else {
        seg = digits[cur];
        leds = knob_mask;
    }
    if (!lit) seg = leds = 0;
    static uint32_t shown_seg, shown_leds = 0xFFFFFFFFu;
    static unsigned shown_cur;
    if (seg == shown_seg && leds == shown_leds && cur == shown_cur) return;   /* no change */
    shown_seg = seg;
    shown_leds = leds;
    shown_cur = cur;
    select_digit(-1);                                   /* no ghosting */
    GPIO4->DR = (GPIO4->DR & ~(SEG_MASK | 0xFFFFu)) | seg | (~leds & 0xFFFFu);   /* LEDs active low */
    if (seg) select_digit((int)cur);
}

void knob_led(int led, bool on)
{
    if (led < 0 || led >= 16) return;
    if (on) knob_mask |= (uint16_t)(1u << led);
    else knob_mask &= (uint16_t)~(1u << led);
}
