#include "ui/controls.h"
#include "ui/pads.h"
#include "fsl_adc.h"
#include "fsl_gpio.h"

#define FSW_PAD_CFG   0xF0B0u   /* 22k pull-up, as the stock */
#define ANALOG_CFG    0x00B0u
#define DEBOUNCE      2u        /* samples of 10 ms */
#define LONG_TICKS    100u      /* 1 s */
#define KNOB_THRESH   48u       /* stock change threshold */

/* A, B, C, D (stock scanner: A = B1_08, B = SD_B0_01, C = SD_B0_02, D = SD_B0_00) */
static GPIO_Type *const fsw_port[FSW_COUNT] = {GPIO2, GPIO3, GPIO3, GPIO3};
static const uint32_t fsw_pin[FSW_COUNT] = {24u, 13u, 14u, 12u};

static struct { uint8_t stable, count; uint16_t held; bool long_sent; } fsw[FSW_COUNT];
static struct { uint8_t sw, ev; } queue[16];
static uint8_t q_head, q_tail;
static uint16_t knobs[KNOB_COUNT], knob_reported[KNOB_COUNT];
static unsigned mux_ch;
static uint32_t last_ms;

static void push(int sw, fsw_event_t ev)
{
    uint8_t next = (uint8_t)((q_head + 1u) % 16u);
    if (next == q_tail) return;
    queue[q_head].sw = (uint8_t)sw;
    queue[q_head].ev = (uint8_t)ev;
    q_head = next;
}

void controls_init(void)
{
    gpio_pin_config_t in = {kGPIO_DigitalInput, 0, kGPIO_NoIntmode};
    gpio_pin_config_t out = {kGPIO_DigitalOutput, 0, kGPIO_NoIntmode};
    pad_set(PAD_B1(8), 5u, FSW_PAD_CFG);
    pad_set(PAD_SD_B0(0), 5u, FSW_PAD_CFG);
    pad_set(PAD_SD_B0(1), 5u, FSW_PAD_CFG);
    pad_set(PAD_SD_B0(2), 5u, FSW_PAD_CFG);
    for (int i = 0; i < FSW_COUNT; i++) GPIO_PinInit(fsw_port[i], fsw_pin[i], &in);

    for (uint32_t i = 1; i <= 3; i++) {            /* mux select: GPIO2_IO17..19 */
        pad_set(PAD_B1(i), 5u, 0x10B0u);
        GPIO_PinInit(GPIO2, 16u + i, &out);
    }
    pad_set(PAD_AD_B0(14), 5u, ANALOG_CFG);        /* ADC1 IN3: k0..k7 */
    pad_set(PAD_AD_B0(15), 5u, ANALOG_CFG);        /* ADC1 IN4: k8..k15 */
    adc_config_t cfg;
    ADC_GetDefaultConfig(&cfg);
    ADC_Init(ADC1, &cfg);
    ADC_SetHardwareAverageConfig(ADC1, kADC_HardwareAverageCount8);
    (void)ADC_DoAutoCalibration(ADC1);
}

uint16_t adc1_read(uint32_t ch)
{
    adc_channel_config_t c = {.channelNumber = ch, .enableInterruptOnConversionCompleted = false};
    ADC_SetChannelConfig(ADC1, 0u, &c);
    for (uint32_t t = 0; t < 100000u && !ADC_GetChannelStatusFlags(ADC1, 0u); t++) {
    }
    return (uint16_t)ADC_GetChannelConversionValue(ADC1, 0u);
}

static uint16_t adc_read(uint32_t ch)
{
    /* Not inverted: the RE notes said the stock uses 4095 - v, but on the
     * pedal that made every knob turn the wrong way (user report). */
    return adc1_read(ch);
}

void controls_task(uint32_t now_ms)
{
    if (now_ms - last_ms < 10u) return;
    last_ms = now_ms;

    for (int i = 0; i < FSW_COUNT; i++) {
        uint8_t down = GPIO_PinRead(fsw_port[i], fsw_pin[i]) == 0u;
        if (down != fsw[i].stable) {
            if (++fsw[i].count >= DEBOUNCE) {
                fsw[i].stable = down;
                fsw[i].count = 0;
                fsw[i].held = 0;
                if (!down && fsw[i].long_sent) fsw[i].long_sent = false;
                push(i, down ? FSW_PRESS : FSW_RELEASE);
            }
        } else {
            fsw[i].count = 0;
            if (down && !fsw[i].long_sent && ++fsw[i].held >= LONG_TICKS) {
                fsw[i].long_sent = true;
                push(i, FSW_LONG);
            }
        }
    }

    /* the select lines settled during the last 10 ms: read, then advance */
    knobs[mux_ch] = adc_read(3u);
    knobs[mux_ch + 8u] = adc_read(4u);
    mux_ch = (mux_ch + 1u) % 8u;
    GPIO_PinWrite(GPIO2, 17u, mux_ch & 1u);
    GPIO_PinWrite(GPIO2, 18u, (mux_ch >> 1) & 1u);
    GPIO_PinWrite(GPIO2, 19u, (mux_ch >> 2) & 1u);
}

bool fsw_down(int sw) { return sw >= 0 && sw < FSW_COUNT && fsw[sw].stable; }

fsw_event_t fsw_event(int *sw)
{
    if (q_tail == q_head) return FSW_NONE;
    *sw = queue[q_tail].sw;
    fsw_event_t ev = (fsw_event_t)queue[q_tail].ev;
    q_tail = (uint8_t)((q_tail + 1u) % 16u);
    return ev;
}

uint16_t knob_value(int k) { return (k >= 0 && k < KNOB_COUNT) ? knobs[k] : 0; }

bool knob_changed(int k)
{
    uint16_t v = knobs[k], r = knob_reported[k];
    uint32_t delta = v > r ? (uint32_t)(v - r) : (uint32_t)(r - v);
    if (delta < KNOB_THRESH) return false;
    knob_reported[k] = v;
    return true;
}
