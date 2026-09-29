#include <string.h>
#include "ui/power.h"
#include "ui/power_logic.h"
#include "ui/pads.h"
#include "ui/controls.h"
#include "ui/display.h"
#include "ui/lightbar.h"
#include "fsl_gpio.h"
#include "fsl_clock.h"
#include "ui/ui.h"
#include "proto/proto.h"
#include "preset/preset.h"
#include "audio/engine.h"
#include "debug/cdc_log.h"
#include "debug/selfupdate.h"
#include "cpu_power.h"

#define ANALOG_CFG 0x00B0u
#define LED_CFG    0x10B0u
#define SUPPLY_FAIL 0xB86u
#define SUPPLY_OK   0xBEAu
#define ACTIVE_PEAK 0.003f        /* chain input |L + R| above ~-50 dBFS = playing */
#define LOW_SHOW_MS 2000u         /* "LOb" on the display for a low-battery warning */
#define PW_MARK 0x5750u           /* "PW" */

static power_state_t st;
static gauge_t gauge;
static idle_t idle;
static uint32_t last_ms, low_until, log_ms, log_every_ms;
static bool blink;
static bool started, sent_charging;
static uint8_t sent_level;
static uint32_t seen_revision;
static bool activity;             /* since the last tick: controls, app, audio */
static uint8_t idle_min, led_pct = 100u;   /* persisted (POWER_SETTINGS_FLASH) */
static enum { MODE_NORMAL, MODE_LOW, MODE_STANDBY, MODE_CRITICAL } mode;

/* status RGB LED on GPIO2_IO0/1/3 (B0_00/01/03), active low: 1 = off */
static void status_led(uint8_t io0, uint8_t io1, uint8_t io3)
{
    GPIO_PinWrite(GPIO2, 0u, io0);
    GPIO_PinWrite(GPIO2, 1u, io1);
    GPIO_PinWrite(GPIO2, 3u, io3);
}

/* Front-panel light for the mode: rings, display, knob LEDs. The knob LED
 * supply (GPIO1_IO2) stays on: it may also feed the knob pots. */
static void apply_mode(void)
{
    bool dark = mode == MODE_STANDBY || mode == MODE_CRITICAL;
    lightbar_set_level(dark ? 0u : led_pct);
    display_set_level(led_pct);
    if (mode == MODE_STANDBY) display_override(" .", 150u, 3000u);      /* a slow dot: still on */
    else if (mode == MODE_CRITICAL) display_override("LOb", 500u, 1000u);
    else if (mode == MODE_LOW) display_override("LOb", LOW_SHOW_MS, LOW_SHOW_MS);
    else display_override(0, 0, 0);
}

static void set_mode(int m)
{
    if ((int)mode == m) return;
    mode = m;
    apply_mode();
}

/* ---- persisted power settings: F:0x80100, after the stock settings in
 * the same sector (flash_store keeps the rest of the sector) ---- */
static void settings_load(void)
{
    const volatile uint8_t *p = (const volatile uint8_t *)(0x60000000u + POWER_SETTINGS_FLASH);
    if ((p[0] | p[1] << 8) != PW_MARK) return;                /* defaults: off, 100 % */
    idle_min = p[2] <= 240u ? p[2] : 0u;
    led_pct = p[3] >= 100u ? 100u : p[3] >= 66u ? 66u : 33u;
}

static int settings_save(void)
{
    uint8_t b[4] = {(uint8_t)PW_MARK, (uint8_t)(PW_MARK >> 8), idle_min, led_pct};
    return flash_store(POWER_SETTINGS_FLASH, b, sizeof b);
}

/* Unused analog blocks and clocks (docs/POWER.md): the second USB PHY and
 * its PLL (the pedal uses USB1 only), the video PLL, and the clock gates of
 * peripherals no code of the app touches. Console `clocks` shows the gates. */
static void power_down_unused(void)
{
    USBPHY2->CTRL_SET = USBPHY_CTRL_CLKGATE_MASK;
    USBPHY2->PWD = 0x001E1C00u;   /* TX/RX power-down bits, as in suspend */
    CCM_ANALOG->PLL_USB2_CLR = CCM_ANALOG_PLL_USB2_EN_USB_CLKS_MASK | CCM_ANALOG_PLL_USB2_ENABLE_MASK;
    CCM_ANALOG->PLL_USB2_CLR = CCM_ANALOG_PLL_USB2_POWER_MASK;
    CCM_ANALOG->PLL_VIDEO_CLR = CCM_ANALOG_PLL_VIDEO_ENABLE_MASK;
    CCM_ANALOG->PLL_VIDEO_SET = CCM_ANALOG_PLL_VIDEO_POWERDOWN_MASK;
    static const clock_ip_name_t kUnused[] = {
        kCLOCK_Can1, kCLOCK_Can1S, kCLOCK_Can2, kCLOCK_Can2S, kCLOCK_Can3, kCLOCK_Can3S,
        kCLOCK_Lpspi1, kCLOCK_Lpspi2, kCLOCK_Lpspi3, kCLOCK_Lpspi4,
        kCLOCK_Enet, kCLOCK_Enet2, kCLOCK_Csi, kCLOCK_Lcd, kCLOCK_LcdPixel, kCLOCK_Pxp,
        kCLOCK_Aoi1, kCLOCK_Aoi2, kCLOCK_Ewm0, kCLOCK_Acmp1, kCLOCK_Acmp2, kCLOCK_Acmp3, kCLOCK_Acmp4,
        kCLOCK_Pwm1, kCLOCK_Pwm2, kCLOCK_Pwm3, kCLOCK_Pwm4,
        kCLOCK_Enc1, kCLOCK_Enc2, kCLOCK_Enc3, kCLOCK_Enc4,
        kCLOCK_Timer1, kCLOCK_Timer2, kCLOCK_Timer3, kCLOCK_Timer4,
        kCLOCK_Kpp, kCLOCK_Spdif, kCLOCK_FlexSpi2, kCLOCK_Flexio1, kCLOCK_Sai2, kCLOCK_Adc2,
        kCLOCK_Lpuart2, kCLOCK_Lpuart3, kCLOCK_Lpuart4, kCLOCK_Lpuart6, kCLOCK_Lpuart7, kCLOCK_Lpuart8,
    };
    for (unsigned i = 0; i < sizeof kUnused / sizeof kUnused[0]; i++) CLOCK_DisableClock(kUnused[i]);
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
    gauge_init(&gauge);
    idle_init(&idle);
    settings_load();
    power_down_unused();
    apply_mode();
}

void power_activity(void) { activity = true; }

static void log_line(void)
{
    uint16_t f = gauge_raw(&gauge);
    log_printf("power: t %lu s batt raw %u filt %u ~%lu mV ~%u%% level %u%s%s\r\n",
               (unsigned long)(last_ms / 1000u), (unsigned)st.battery_raw, (unsigned)f,
               (unsigned long)gauge_mv(f), gauge_percent(f), (unsigned)st.level,
               st.charging ? " charging" : "", st.critical ? " CRITICAL" : "");
}

void power_task(uint32_t now_ms)
{
    /* between ticks: controls and app edits wake the panel at once */
    if (ui_revision() != seen_revision) { seen_revision = ui_revision(); activity = true; }
    if (activity && idle_activity(&idle) && !gauge.critical) set_mode(low_until ? MODE_LOW : MODE_NORMAL);
    if (now_ms - last_ms < POWER_TICK_MS) return;
    last_ms = now_ms;
    st.battery_raw = adc1_read(9u);
    st.supply_raw = adc1_read(7u);
    st.charging = GPIO_PinRead(GPIO1, 19u) != 0u;
    gauge_event_t ev = gauge_update(&gauge, st.battery_raw, st.charging);
    st.level = gauge.level;
    st.percent = (uint8_t)gauge_percent(gauge_raw(&gauge));
    st.critical = gauge.critical;
    /* stock 0x18a5e: BB to the app when the level or the charger changes */
    if (started && (st.level != sent_level || st.charging != sent_charging)) proto_notify_battery();
    sent_level = st.level;
    sent_charging = st.charging;
    started = true;
    bool was_low = st.supply_low;
    if (st.supply_raw < SUPPLY_FAIL) st.supply_low = true;
    else if (st.supply_raw > SUPPLY_OK) st.supply_low = false;
    if (st.supply_low != was_low) power_fail_changed(st.supply_low);

    /* critical battery: save the settings while there is charge, then keep
     * the flash untouched (a brown-out in a sector erase loses the sector)
     * and the panel dark; the audio keeps running */
    if (ev == GAUGE_EV_CRITICAL) {
        log_printf("power: battery critical (raw %u): settings saved, panel off, flash writes off\r\n",
                   (unsigned)gauge_raw(&gauge));
        ui_flush_settings();
        flash_store_block(1);
    } else if (ev == GAUGE_EV_RECOVERED) {
        flash_store_block(0);
        log_printf("power: battery ok again\r\n");
    } else if (ev == GAUGE_EV_LOW) {
        log_printf("power: battery low - charge now\r\n");
        low_until = now_ms + LOW_SHOW_MS;
    }

    bool playing = engine_input_peak() > ACTIVE_PEAK;
    idle_tick(&idle, POWER_TICK_MS, idle_min, activity || playing, st.charging);
    activity = false;
    if (gauge.critical) set_mode(MODE_CRITICAL);
    else if (idle.standby) set_mode(MODE_STANDBY);
    else if (low_until && (int32_t)(now_ms - low_until) < 0) set_mode(MODE_LOW);
    else { low_until = 0; set_mode(MODE_NORMAL); }

    if (log_every_ms && now_ms - log_ms >= log_every_ms) {
        log_ms = now_ms;
        log_line();
    }

    blink = !blink;
    /* Charging: LED off from the MCU, exactly as the stock (0x1897c drives
     * GPIO2 IO0/1/3 all high = off while GPIO1_IO19 is high). The manual's
     * solid red (charging) / solid green (full) must then come from the
     * charger chip, not from the firmware.
     * TODO(pedal): charge with the open firmware and look at the status LED:
     * red while charging, green when full = nothing to do; dark = the stock
     * does drive it somewhere else (then io0 = red, io1 = green, as the
     * battery colours below). */
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

/* console `power ...` (docs/POWER.md) */
static int parse_u(const char *s, unsigned *out)
{
    unsigned v = 0;
    if (!s || !*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000u) return 0;
        v = v * 10u + (unsigned)(*s - '0');
    }
    *out = v;
    return 1;
}

static int streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

void power_console(int argc, char **argv)
{
    unsigned v = 0;
    const char *what = argc > 1 ? argv[1] : "";
    bool ok = argc > 2 && parse_u(argv[2], &v);
    if (argc <= 1) {
        uint16_t f = gauge_raw(&gauge);
        log_printf("power: battery=%u (level %u) supply=%u%s charging=%u\r\n",
                   (unsigned)st.battery_raw, (unsigned)st.level, (unsigned)st.supply_raw,
                   st.supply_low ? " LOW" : "", (unsigned)st.charging);
        log_printf("power: filtered %u ~%lu mV (est.) ~%u%% (est.)%s mode %s\r\n", (unsigned)f,
                   (unsigned long)gauge_mv(f), gauge_percent(f), st.critical ? " CRITICAL" : "",
                   mode == MODE_STANDBY ? "standby" : mode == MODE_CRITICAL ? "critical"
                   : mode == MODE_LOW ? "low" : "normal");
        log_printf("power: sleep %s, clock %u MHz, VDD_SOC %u mV, led %u%%, idle %u min%s, log %lu s\r\n",
                   cpu_sleep_enabled() ? "on" : "off", cpu_clock_mhz(), cpu_vdd_soc_mv(),
                   (unsigned)led_pct, (unsigned)idle_min, idle_min ? "" : " (off)",
                   (unsigned long)(log_every_ms / 1000u));
    } else if (streq(what, "sleep") && argc > 2) {
        cpu_sleep_enable(streq(argv[2], "on"));
        log_printf("power sleep %s\r\n", cpu_sleep_enabled() ? "on" : "off");
    } else if (streq(what, "clock") && ok) {
        int r = cpu_set_clock(v);
        log_printf("power clock %u: %s (core %u MHz, VDD_SOC %u mV)\r\n", v, r == 0 ? "ok" : "refused",
                   cpu_clock_mhz(), cpu_vdd_soc_mv());
    } else if (streq(what, "led") && ok && (v == 100u || v == 66u || v == 33u)) {
        led_pct = (uint8_t)v;
        apply_mode();
        log_printf("power led %u%%: %s\r\n", v, settings_save() == 0 ? "saved" : "NOT saved");
    } else if (streq(what, "idle") && ok && v <= 240u) {
        idle_min = (uint8_t)v;
        idle_activity(&idle);
        log_printf("power idle %u min%s: %s\r\n", v, v ? "" : " (off)",
                   settings_save() == 0 ? "saved" : "NOT saved");
    } else if (streq(what, "log") && ok) {
        log_every_ms = v * 1000u;
        log_printf("power log every %u s%s\r\n", v, v ? "" : " (off)");
    } else {
        log_printf("usage: power [sleep on|off | clock 600|528|396 | led 100|66|33 |"
                   " idle <min, 0 off> | log <s, 0 off>]\r\n");
    }
}
