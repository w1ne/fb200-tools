/* SAI1 + eDMA audio path. The SAI is the I2S master (MCLK 12.288 MHz =
 * 256*fs at 44.1 kHz, BCLK 1.4112 MHz, 16-bit stereo); the NAU88L21 codec is
 * the clock slave. TX drives BCLK/FS and RX is synchronous with TX, so both
 * directions share one clock pair.
 *
 * Pads recovered from the stock image (docs/AUDIO_PATH.md): MCLK =
 * GPIO_SD_B1_03, RX_DATA00 = GPIO_AD_B1_12, TX_DATA00 = GPIO_AD_B1_13,
 * TX_BCLK = GPIO_AD_B1_14, TX_SYNC = GPIO_AD_B1_15, all ALT3, pad config
 * 0x10B0 (same value the stock uses). */
#include "fsl_clock.h"
#include "cold.h"
#include "fsl_common.h"
#include "fsl_dmamux.h"
#include "fsl_edma.h"
#include "fsl_iomuxc.h"
#include "fsl_sai.h"
#include "fsl_sai_edma.h"
#include "audio/sai.h"
#include "audio/audio_config.h"

#define SAI_BASE SAI1
#define SAI_DMA DMA0
#define SAI_RX_CH 0u
#define SAI_TX_CH 1u
#define SAI_RX_REQ kDmaRequestMuxSai1Rx
#define SAI_TX_REQ kDmaRequestMuxSai1Tx
#define SAI_SAMPLE_RATE AUDIO_FS
#define SAI_MCLK_HZ AUDIO_MCLK_HZ /* 256 * fs */
#define SAI_PAD_CFG 0x10B0u

static edma_handle_t s_rx_dma, s_tx_dma;
static sai_edma_handle_t s_rx, s_tx;
static int16_t s_rx_buf[2][SAI_BLOCK_FRAMES * 2];
static int16_t s_tx_buf[2][SAI_BLOCK_FRAMES * 2];
static uint8_t s_rx_idx, s_tx_idx;

#define RING_FRAMES 512
static int16_t s_rx_ring[RING_FRAMES * 2];
static uint32_t s_rx_head, s_rx_tail;
static int16_t s_tx_ring[RING_FRAMES * 2];
static uint32_t s_tx_head, s_tx_tail;
static volatile uint32_t s_rx_blocks, s_tx_blocks, s_over, s_under;

static void sai_clock_init(void)
{
    /* The stock clock tree: PLL4 = 24 MHz * (30 + 66/625) = 722.5344 MHz,
     * SAI1 root = PLL4 / 1 / 64 = 11.2896 MHz = 256 * 44.1 kHz. */
    const clock_audio_pll_config_t pll = {
        .loopDivider = 30,
        .postDivider = 1,
        .numerator = 66,
        .denominator = 625,
        .src = 0,
    };
    CLOCK_InitAudioPll(&pll);
    CLOCK_SetMux(kCLOCK_Sai1Mux, 2);     /* PLL4 */
    CLOCK_SetDiv(kCLOCK_Sai1PreDiv, 0);  /* /1 */
    CLOCK_SetDiv(kCLOCK_Sai1Div, 63);    /* /64 -> 11.2896 MHz */
}

static void sai_pads(void)
{
    /* MCLK is an output only with IOMUXC_GPR1[19] set (the stock sets it).
     * Without it the codec had no MCLK: ADC zeros, DAC silent. */
    IOMUXC_EnableMode(IOMUXC_GPR, kIOMUXC_GPR_SAI1MClkOutputDir, true);
    IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B1_03_SAI1_MCLK, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_12_SAI1_RX_DATA00, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_13_SAI1_TX_DATA00, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_14_SAI1_TX_BCLK, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B1_15_SAI1_TX_SYNC, 0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_SD_B1_03_SAI1_MCLK, SAI_PAD_CFG);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_12_SAI1_RX_DATA00, SAI_PAD_CFG);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_13_SAI1_TX_DATA00, SAI_PAD_CFG);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_14_SAI1_TX_BCLK, SAI_PAD_CFG);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_AD_B1_15_SAI1_TX_SYNC, SAI_PAD_CFG);
}

/* eDMA ping-pong. Both buffers stay queued in the SDK handle; each completion
 * callback refills the buffer that just finished (completion order is 0, 1,
 * 0, 1, ...) and requeues it while the other one plays. The SDK reports
 * kStatus_SAI_{Rx,Tx}Busy (more queued) or ..Idle (queue drained, DMA
 * stopped) and never kStatus_Success: the first version filtered on Success,
 * never requeued, and the SDK switched the DMA request off after one block
 * (TCSR.FRDE=0, FIFO underrun; read on the pedal). */
static void rx_done(I2S_Type *base, sai_edma_handle_t *handle, status_t status,
                    void *userData)
{
    (void)handle;
    (void)userData;
    (void)status;
    const int16_t *buf = s_rx_buf[s_rx_idx];
    for (size_t i = 0; i < SAI_BLOCK_FRAMES; i++) {
        uint32_t next = (s_rx_head + 1u) % RING_FRAMES;
        if (next == s_rx_tail) {
            s_over++;
            break;
        }
        s_rx_ring[s_rx_head * 2 + 0] = buf[i * 2 + 0];
        s_rx_ring[s_rx_head * 2 + 1] = buf[i * 2 + 1];
        s_rx_head = next;
    }
    s_rx_blocks++;
    sai_transfer_t xfer = {
        .data = (uint8_t *)s_rx_buf[s_rx_idx],
        .dataSize = sizeof(s_rx_buf[0]),
    };
    s_rx_idx ^= 1u;
    (void)SAI_TransferReceiveEDMA(base, &s_rx, &xfer);
}

static void tx_done(I2S_Type *base, sai_edma_handle_t *handle, status_t status,
                    void *userData)
{
    (void)handle;
    (void)userData;
    (void)status;
    int16_t *buf = s_tx_buf[s_tx_idx];
    for (size_t i = 0; i < SAI_BLOCK_FRAMES; i++) {
        if (s_tx_tail == s_tx_head) {
            buf[i * 2 + 0] = 0;
            buf[i * 2 + 1] = 0;
            s_under++;
        } else {
            buf[i * 2 + 0] = s_tx_ring[s_tx_tail * 2 + 0];
            buf[i * 2 + 1] = s_tx_ring[s_tx_tail * 2 + 1];
            s_tx_tail = (s_tx_tail + 1u) % RING_FRAMES;
        }
    }
    s_tx_blocks++;
    sai_transfer_t xfer = {
        .data = (uint8_t *)s_tx_buf[s_tx_idx],
        .dataSize = sizeof(s_tx_buf[0]),
    };
    s_tx_idx ^= 1u;
    (void)SAI_TransferSendEDMA(base, &s_tx, &xfer);
}

COLD void sai_audio_init(void)
{
    sai_clock_init();
    sai_pads();

    sai_transceiver_t cfg;
    SAI_GetClassicI2SConfig(&cfg, kSAI_WordWidth16bits, kSAI_Stereo,
                            kSAI_Channel0Mask);
    cfg.masterSlave = kSAI_Master;
    cfg.frameSync.frameSyncWidth = 16;
    cfg.syncMode = kSAI_ModeAsync;
    SAI_Init(SAI_BASE);

    DMAMUX_Init(DMAMUX);
    DMAMUX_SetSource(DMAMUX, SAI_RX_CH, (uint8_t)SAI_RX_REQ);
    DMAMUX_EnableChannel(DMAMUX, SAI_RX_CH);
    DMAMUX_SetSource(DMAMUX, SAI_TX_CH, (uint8_t)SAI_TX_REQ);
    DMAMUX_EnableChannel(DMAMUX, SAI_TX_CH);

    edma_config_t dcfg;
    EDMA_GetDefaultConfig(&dcfg);
    EDMA_Init(SAI_DMA, &dcfg);
    EDMA_CreateHandle(&s_rx_dma, SAI_DMA, SAI_RX_CH);
    EDMA_CreateHandle(&s_tx_dma, SAI_DMA, SAI_TX_CH);
    SAI_TransferRxCreateHandleEDMA(SAI_BASE, &s_rx, rx_done, NULL, &s_rx_dma);
    SAI_TransferTxCreateHandleEDMA(SAI_BASE, &s_tx, tx_done, NULL, &s_tx_dma);

    /* The EDMA variants also set the handle's bytes-per-frame and FIFO
     * watermark; plain SAI_{Tx,Rx}SetConfig left them 0, so every TCD had
     * NBYTES=0/BITER=0 and eDMA flagged a configuration error (ES=0x80000108,
     * read on the pedal) while the SAI FIFO underran. */
    SAI_TransferTxSetConfigEDMA(SAI_BASE, &s_tx, &cfg);
    cfg.syncMode = kSAI_ModeSync; /* RX shares the TX bit clock/frame sync */
    SAI_TransferRxSetConfigEDMA(SAI_BASE, &s_rx, &cfg);
    SAI_TxSetBitClockRate(SAI_BASE, SAI_MCLK_HZ, SAI_SAMPLE_RATE, 16, 2);
    SAI_RxSetBitClockRate(SAI_BASE, SAI_MCLK_HZ, SAI_SAMPLE_RATE, 16, 2);

    /* Prime the transmit buffers with silence so the DAC starts clean. */
    for (size_t i = 0; i < 2; i++) {
        for (size_t j = 0; j < SAI_BLOCK_FRAMES * 2; j++) {
            s_tx_buf[i][j] = 0;
        }
    }

    /* Queue both halves; index = the buffer that completes next. */
    for (size_t i = 0; i < 2; i++) {
        sai_transfer_t xfer = {.data = (uint8_t *)s_rx_buf[i], .dataSize = sizeof(s_rx_buf[0])};
        (void)SAI_TransferReceiveEDMA(SAI_BASE, &s_rx, &xfer);
        xfer = (sai_transfer_t){.data = (uint8_t *)s_tx_buf[i], .dataSize = sizeof(s_tx_buf[0])};
        (void)SAI_TransferSendEDMA(SAI_BASE, &s_tx, &xfer);
    }
    s_rx_idx = 0;
    s_tx_idx = 0;

    SAI_TxEnable(SAI_BASE, true);
    SAI_RxEnable(SAI_BASE, true);
}

size_t sai_pull(int16_t *dst, size_t frames)
{
    size_t n = 0;
    while (n < frames && s_rx_tail != s_rx_head) {
        dst[n * 2 + 0] = s_rx_ring[s_rx_tail * 2 + 0];
        dst[n * 2 + 1] = s_rx_ring[s_rx_tail * 2 + 1];
        s_rx_tail = (s_rx_tail + 1u) % RING_FRAMES;
        n++;
    }
    return n;
}

size_t sai_push(const int16_t *src, size_t frames)
{
    size_t n = 0;
    while (n < frames) {
        uint32_t next = (s_tx_head + 1u) % RING_FRAMES;
        if (next == s_tx_tail) {
            break;
        }
        s_tx_ring[s_tx_head * 2 + 0] = src[n * 2 + 0];
        s_tx_ring[s_tx_head * 2 + 1] = src[n * 2 + 1];
        s_tx_head = next;
        n++;
    }
    return n;
}

uint32_t sai_rx_fill(void)
{
    return (s_rx_head + RING_FRAMES - s_rx_tail) % RING_FRAMES;
}

uint32_t sai_tx_fill(void)
{
    return (s_tx_head + RING_FRAMES - s_tx_tail) % RING_FRAMES;
}

void sai_stats(uint32_t *rx_fill, uint32_t *tx_fill, uint32_t *rx_blocks,
               uint32_t *tx_blocks, uint32_t *over, uint32_t *under)
{
    *rx_fill = (s_rx_head + RING_FRAMES - s_rx_tail) % RING_FRAMES;
    *tx_fill = (s_tx_head + RING_FRAMES - s_tx_tail) % RING_FRAMES;
    *rx_blocks = s_rx_blocks;
    *tx_blocks = s_tx_blocks;
    *over = s_over;
    *under = s_under;
}
