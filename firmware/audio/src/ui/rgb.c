#include "ui/rgb.h"
#include "ui/pads.h"
#include "fsl_clock.h"
#include "fsl_dmamux.h"
#include "fsl_edma.h"
#include "fsl_flexio_uart_edma.h"

#define RGB_DMA_CH 2u                     /* 0/1 are SAI1 RX/TX */
#define FLEXIO_HZ  80000000u              /* PLL3 480 MHz / 6 */
#define BAUD       6666666u               /* 150 ns per UART bit */

static FLEXIO_UART_Type uart = {
    .flexioBase = FLEXIO2, .TxPinIndex = 2u, .RxPinIndex = 31u,
    .shifterIndex = {0u, 1u}, .timerIndex = {0u, 1u},
};
static flexio_uart_edma_handle_t handle;
static edma_handle_t dma;
static uint8_t pixels[RGB_COUNT][3];
static uint8_t wire[RGB_COUNT * 24];
static volatile bool busy;
static bool inverted = true;
static uint8_t sym[2] = {0xFC, 0xC0};     /* 0: ~450 ns high, 1: ~1.05 us (inverted) */

static void done(FLEXIO_UART_Type *b, flexio_uart_edma_handle_t *h, status_t s, void *u)
{
    (void)b; (void)h; (void)s; (void)u;
    busy = false;
}

static void apply_polarity(void)
{
    if (inverted) FLEXIO2->SHIFTCTL[0] |= FLEXIO_SHIFTCTL_PINPOL_MASK;
    else FLEXIO2->SHIFTCTL[0] &= ~FLEXIO_SHIFTCTL_PINPOL_MASK;
}

void rgb_init(void)
{
    CLOCK_SetMux(kCLOCK_Flexio2Mux, 3u);      /* PLL3 SW (480 MHz) */
    CLOCK_SetDiv(kCLOCK_Flexio2PreDiv, 5u);   /* /6 */
    CLOCK_SetDiv(kCLOCK_Flexio2Div, 0u);      /* /1 -> 80 MHz */
    pad_set(PAD_B0(2), 4u, 0x10B1u);          /* ALT4 FLEXIO2_D02, stock pad config */

    flexio_uart_config_t cfg;
    FLEXIO_UART_GetDefaultConfig(&cfg);
    cfg.baudRate_Bps = BAUD;
    cfg.enableUart = true;
    cfg.enableInDoze = true;
    cfg.enableInDebug = true;
    (void)FLEXIO_UART_Init(&uart, &cfg, FLEXIO_HZ);
    apply_polarity();

    DMAMUX_SetSource(DMAMUX, RGB_DMA_CH, (uint8_t)kDmaRequestMuxFlexIO2Request0Request1);
    DMAMUX_EnableChannel(DMAMUX, RGB_DMA_CH);
    EDMA_CreateHandle(&dma, DMA0, RGB_DMA_CH);
    (void)FLEXIO_UART_TransferCreateHandleEDMA(&uart, &handle, done, NULL, &dma, NULL);
    rgb_fill(0, 0, 0);
    rgb_show();
}

void rgb_config(bool invert, uint8_t sym0, uint8_t sym1)
{
    inverted = invert;
    sym[0] = sym0;
    sym[1] = sym1;
    apply_polarity();
}

void rgb_set(int i, uint8_t r, uint8_t g, uint8_t b)
{
    if (i < 0 || i >= RGB_COUNT) return;
    pixels[i][0] = g;                         /* WS2812 order: G, R, B */
    pixels[i][1] = r;
    pixels[i][2] = b;
}

void rgb_fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < RGB_COUNT; i++) rgb_set(i, r, g, b);
}

bool rgb_show(void)
{
    if (busy) return false;
    uint8_t *w = wire;
    for (int i = 0; i < RGB_COUNT; i++)
        for (int c = 0; c < 3; c++)
            for (int bit = 7; bit >= 0; bit--) *w++ = sym[(pixels[i][c] >> bit) & 1u];
    flexio_uart_transfer_t xfer = {.data = wire, .dataSize = sizeof wire};
    busy = true;
    if (FLEXIO_UART_TransferSendEDMA(&uart, &handle, &xfer) != kStatus_Success) busy = false;
    return busy;
}
