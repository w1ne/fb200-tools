/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#include <string.h>
#include "cold.h"
#include "audio/bt_audio.h"
#include "audio/audio_config.h"
#include "fsl_clock.h"
#include "fsl_dmamux.h"
#include "fsl_edma.h"
#include "fsl_iomuxc.h"
#include "fsl_sai_edma.h"

#define BT_DMA_CH   3u              /* 0/1 SAI1, 2 RGB LEDs */
#define BLOCK       32u             /* frames per DMA block */
#define RING        512u
#define PAD_CFG     0x10B0u

static edma_handle_t dma;
static sai_edma_handle_t rx;
static int32_t buf[2][BLOCK * 2];   /* DTCM: not cached, eDMA-visible */
static int16_t ring[RING * 2];
static volatile uint32_t head, tail, blocks;
static volatile int32_t peak;
static unsigned idx;

static void rx_done(I2S_Type *base, sai_edma_handle_t *h, status_t s, void *u)
{
    (void)h; (void)s; (void)u;
    const int32_t *b = buf[idx];
    for (uint32_t i = 0; i < BLOCK; i++) {
        uint32_t next = (head + 1u) % RING;
        if (next == tail) break;                         /* engine not pulling: drop */
        int16_t l = (int16_t)(b[i * 2 + 0] >> 16), r = (int16_t)(b[i * 2 + 1] >> 16);
        ring[head * 2 + 0] = l;
        ring[head * 2 + 1] = r;
        head = next;
        int32_t a = l < 0 ? -l : l;
        if (a > peak) peak = a;
    }
    blocks++;
    sai_transfer_t x = {.data = (uint8_t *)buf[idx], .dataSize = sizeof buf[0]};
    idx ^= 1u;
    (void)SAI_TransferReceiveEDMA(base, &rx, &x);
}

COLD void bt_audio_init(void)
{
    CLOCK_SetMux(kCLOCK_Sai3Mux, 2);        /* PLL4 (set up by sai.c) */
    CLOCK_SetDiv(kCLOCK_Sai3PreDiv, 0);     /* /1 */
    CLOCK_SetDiv(kCLOCK_Sai3Div, 63);       /* /64 -> 11.2896 MHz, as SAI1 */
    IOMUXC_SetPinMux(IOMUXC_GPIO_EMC_33_SAI3_RX_DATA, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_EMC_38_SAI3_TX_BCLK, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_EMC_39_SAI3_TX_SYNC, 1U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_EMC_33_SAI3_RX_DATA, PAD_CFG);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_EMC_38_SAI3_TX_BCLK, PAD_CFG);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_EMC_39_SAI3_TX_SYNC, PAD_CFG);

    sai_transceiver_t cfg;
    SAI_GetClassicI2SConfig(&cfg, kSAI_WordWidth32bits, kSAI_Stereo, kSAI_Channel0Mask);
    cfg.masterSlave = kSAI_Master;
    SAI_Init(SAI3);
    SAI_TxSetConfig(SAI3, &cfg);            /* TX only generates BCLK/FS */
    SAI3->TCR3 &= ~I2S_TCR3_TCE_MASK;       /* no TX data pin */
    SAI_TxSetBitClockRate(SAI3, AUDIO_MCLK_HZ, AUDIO_FS, 32, 2);

    DMAMUX_SetSource(DMAMUX, BT_DMA_CH, (uint8_t)kDmaRequestMuxSai3Rx);
    DMAMUX_EnableChannel(DMAMUX, BT_DMA_CH);
    EDMA_CreateHandle(&dma, DMA0, BT_DMA_CH);
    SAI_TransferRxCreateHandleEDMA(SAI3, &rx, rx_done, NULL, &dma);
    cfg.syncMode = kSAI_ModeSync;           /* RX on the TX clocks */
    SAI_TransferRxSetConfigEDMA(SAI3, &rx, &cfg);
    SAI_RxSetBitClockRate(SAI3, AUDIO_MCLK_HZ, AUDIO_FS, 32, 2);
    for (unsigned i = 0; i < 2; i++) {
        sai_transfer_t x = {.data = (uint8_t *)buf[i], .dataSize = sizeof buf[0]};
        (void)SAI_TransferReceiveEDMA(SAI3, &rx, &x);
    }
    idx = 0;
    SAI_TxEnable(SAI3, true);
    SAI_RxEnable(SAI3, true);
}

size_t bt_audio_pull(float *l, float *r, size_t frames)
{
    size_t n = 0;
    while (n < frames && tail != head) {
        l[n] = (float)ring[tail * 2 + 0] * (1.0f / 32768.0f);
        r[n] = (float)ring[tail * 2 + 1] * (1.0f / 32768.0f);
        tail = (tail + 1u) % RING;
        n++;
    }
    /* keep latency bounded if the BT clock runs slightly fast */
    while (((head + RING - tail) % RING) > 4u * BLOCK) tail = (tail + 1u) % RING;
    return n;
}

void bt_audio_stats(uint32_t *b, uint32_t *fill, int32_t *pk)
{
    *b = blocks;
    *fill = (head + RING - tail) % RING;
    *pk = peak;
    peak = 0;
}
