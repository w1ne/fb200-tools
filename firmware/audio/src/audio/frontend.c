#include "audio/frontend.h"
#include "fsl_gpio.h"
#include "fsl_iomuxc.h"

#define PAD_CFG 0x10B0u

static void out_pin(uint32_t mux, uint32_t a, uint32_t b, uint32_t c, uint32_t cfg,
                    uint32_t pin, uint8_t level)
{
    IOMUXC_SetPinMux(mux, a, b, c, cfg, 0U);
    IOMUXC_SetPinConfig(mux, a, b, c, cfg, PAD_CFG);
    gpio_pin_config_t pc = {kGPIO_DigitalOutput, level, kGPIO_NoIntmode};
    GPIO_PinInit(GPIO2, pin, &pc);
}

void frontend_init(void)
{
    out_pin(IOMUXC_GPIO_B1_15_GPIO2_IO31, 31u, 1u);
    out_pin(IOMUXC_GPIO_B1_10_GPIO2_IO26, 26u, 0u);
    out_pin(IOMUXC_GPIO_B1_09_GPIO2_IO25, 25u, 1u);
    out_pin(IOMUXC_GPIO_B1_11_GPIO2_IO27, 27u, 1u);
    /* GPIO1_IO2 (AD_B0_02) high: supply enable of the knob LEDs (verified on
     * the pedal with the camera: GPIO4 drove them, nothing lit until this). */
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_02_GPIO1_IO02, 0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B0_02_GPIO1_IO02, PAD_CFG);
    gpio_pin_config_t on = {kGPIO_DigitalOutput, 1, kGPIO_NoIntmode};
    GPIO_PinInit(GPIO1, 2u, &on);
}

void frontend_enable(void)
{
    GPIO_PinWrite(GPIO2, 31u, 0u);
    GPIO_PinWrite(GPIO2, 26u, 1u);
}
