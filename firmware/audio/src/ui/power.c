#include "ui/power.h"
#include "ui/pads.h"
#include "ui/controls.h"
#include "fsl_gpio.h"
#include "ui/ui.h"
#include "proto/proto.h"

#define ANALOG_CFG 0x00B0u
#define LED_CFG    0x10B0u
/* stock thresholds (ITCM 0x188dc, 0x19534) */
static const uint16_t kLevel[4] = {0xE74u, 0xE10u, 0xD98u, 0xC80u};   /* >= -> 4,3,2,1 */
#define SUPPLY_FAIL 0xB86u
#define SUPPLY_OK   0xBEAu

static power_state_t st;
static uint32_t last_ms;
static bool blink;
/* A new level counts after LEVEL_READS equal readings in a row (the stock
 * 0x18940 wants 100 at its own rate; ours are 250 ms apart). */
#define LEVEL_READS 8u
static uint8_t pending, pending_n, sent_level;
static bool started, sent_charging;

/* status RGB LED on GPIO2_IO0/1/3 (B0_00/01/03), active low: 1 = off */
static void status_led(uint8_t io0, uint8_t io1, uint8_t io3)
{
    GPIO_PinWrite(GPIO2, 0u, io0);
    GPIO_PinWrite(GPIO2, 1u, io1);
    GPIO_PinWrite(GPIO2, 3u, io3);
}

void power_init(void)
{
    gpio_pin_config_t in = {kGPIO_DigitalInput, 0, kGPIO_NoIntmode};
    gpio_pin_config_t off = {kGPIO_DigitalOutput, 1, kGPIO_NoIntmode};
    pad_set(58u + 2u, 5u, ANALOG_CFG);      /* AD_B1_02: ADC1 IN7 */
    pad_set(58u + 4u, 5u, ANALOG_CFG);      /* AD_B1_04: ADC1 IN9 */
    pad_set(58u + 3u, 5u, 0xF0B0u);         /* AD_B1_03: GPIO1_IO19 charger */
    GPIO_PinInit(GPIO1, 19u, &in);
    for (uint32_t i = 0; i <= 3; i++) {
        if (i == 2) continue;               /* B0_02 is the RGB LED chain */
        pad_set(PAD_B0(i), 5u, LED_CFG);
        GPIO_PinInit(GPIO2, i, &off);
    }
}

void power_task(uint32_t now_ms)
{
    if (now_ms - last_ms < 250u) return;
    last_ms = now_ms;
    st.battery_raw = adc1_read(9u);
    st.supply_raw = adc1_read(7u);
    st.charging = GPIO_PinRead(GPIO1, 19u) != 0u;
    uint8_t lvl = 0;
    while (lvl < 4 && st.battery_raw < kLevel[lvl]) lvl++;
    lvl = st.charging ? 4u : (uint8_t)(4u - lvl);   /* stock 0x1897c: level 4 while charging */
    if (!started) st.level = lvl;
    else if (lvl == st.level) pending_n = 0;
    else if (lvl != pending) { pending = lvl; pending_n = 1; }
    else if (++pending_n >= LEVEL_READS) st.level = lvl;
    /* stock 0x18a5e: BB to the app when the level or the charger changes */
    if (started && (st.level != sent_level || st.charging != sent_charging)) proto_notify_battery();
    sent_level = st.level;
    sent_charging = st.charging;
    started = true;
    bool was_low = st.supply_low;
    if (st.supply_raw < SUPPLY_FAIL) st.supply_low = true;
    else if (st.supply_raw > SUPPLY_OK) st.supply_low = false;
    if (st.supply_low != was_low) power_fail_changed(st.supply_low);

    blink = !blink;
    if (st.charging) status_led(1, 1, 1);
    else if (st.level >= 2) status_led(1, 0, 1);
    else if (st.level == 1) status_led(0, 0, 1);
    else status_led(blink, 1, 1);
}

const power_state_t *power_state(void) { return &st; }

/* The stock power-fail path (ITCM 0x19534): the supply sense on ADC1 IN7
 * drops when the power switch is turned off; the stock then saves its
 * settings and flips the GPIO_B1_10 / B1_15 pair (the power latch), and
 * flips it back if the sense recovers (e.g. USB still connected). */
void power_fail_changed(bool low)
{
    if (low) {
        ui_flush_settings();
        GPIO_PinWrite(GPIO2, 31u, 1u);   /* B1_15 */
        GPIO_PinWrite(GPIO2, 26u, 0u);   /* B1_10 */
    } else {
        GPIO_PinWrite(GPIO2, 31u, 0u);   /* running state, see frontend.c */
        GPIO_PinWrite(GPIO2, 26u, 1u);
    }
}
