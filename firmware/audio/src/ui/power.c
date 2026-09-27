#include "ui/power.h"
#include "ui/pads.h"
#include "ui/controls.h"
#include "fsl_gpio.h"

#define ANALOG_CFG 0x00B0u
#define LED_CFG    0x10B0u
/* stock thresholds (ITCM 0x188dc, 0x19534) */
static const uint16_t kLevel[4] = {0xE74u, 0xE10u, 0xD98u, 0xC80u};   /* >= -> 4,3,2,1 */
#define SUPPLY_FAIL 0xB86u
#define SUPPLY_OK   0xBEAu

static power_state_t st;
static uint32_t last_ms;
static bool blink;

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
    st.level = (uint8_t)(4u - lvl);
    if (st.supply_raw < SUPPLY_FAIL) st.supply_low = true;
    else if (st.supply_raw > SUPPLY_OK) st.supply_low = false;

    blink = !blink;
    if (st.charging) status_led(1, 1, 1);
    else if (st.level >= 2) status_led(1, 0, 1);
    else if (st.level == 1) status_led(0, 0, 1);
    else status_led(blink, 1, 1);
}

const power_state_t *power_state(void) { return &st; }
