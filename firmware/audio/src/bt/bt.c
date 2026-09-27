#include <string.h>
#include "bt/bt.h"
#include "debug/cdc_log.h"
#include "fsl_clock.h"
#include "fsl_iomuxc.h"
#include "fsl_lpuart.h"

#define BAUD 115200u
#define PAD_CFG 0x10B0u

static lpuart_handle_t handle;
static uint8_t rx_ring[512];
static uint8_t tx_buf[256];
static volatile int tx_busy;
static uint8_t log_buf[128];
static size_t log_len;
static int step = -1;               /* AT start-up step, -1 = not started */
static uint32_t next_ms;
static uint32_t rx_total;

__attribute__((weak)) void bt_rx_frame_bytes(const uint8_t *data, size_t n) { (void)data; (void)n; }

static void cb(LPUART_Type *b, lpuart_handle_t *h, status_t s, void *u)
{
    (void)b; (void)h; (void)u;
    if (s == kStatus_LPUART_TxIdle) tx_busy = 0;
}

void bt_init(void)
{
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_12_LPUART5_TX, 0U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_B1_13_LPUART5_RX, 0U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_12_LPUART5_TX, PAD_CFG);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_B1_13_LPUART5_RX, PAD_CFG);
    /* UART clock root: PLL3 / 6 / (UART_PODF + 1) (80 MHz with the BSP setup) */
    uint32_t clk = (CLOCK_GetPllFreq(kCLOCK_PllUsb1) / 6u) / (CLOCK_GetDiv(kCLOCK_UartDiv) + 1u);
    lpuart_config_t cfg;
    LPUART_GetDefaultConfig(&cfg);
    cfg.baudRate_Bps = BAUD;
    cfg.enableTx = true;
    cfg.enableRx = true;
    (void)LPUART_Init(LPUART5, &cfg, clk);
    LPUART_TransferCreateHandle(LPUART5, &handle, cb, NULL);
    LPUART_TransferStartRingBuffer(LPUART5, &handle, rx_ring, sizeof rx_ring);
    step = 0;
    next_ms = 0;
}

int bt_send(const uint8_t *data, size_t n)
{
    if (tx_busy || n > sizeof tx_buf) return -1;
    memcpy(tx_buf, data, n);
    lpuart_transfer_t x = {.data = tx_buf, .dataSize = n};
    tx_busy = 1;
    if (LPUART_TransferSendNonBlocking(LPUART5, &handle, &x) != kStatus_Success) { tx_busy = 0; return -1; }
    return 0;
}

int bt_at(const char *cmd)
{
    uint8_t b[64];
    size_t n = strlen(cmd);
    if (n + 2 > sizeof b) return -1;
    memcpy(b, cmd, n);
    b[n++] = '\r';
    b[n++] = '\n';
    return bt_send(b, n);
}

/* The stock start-up sequence (ITCM 0x1b630), minus the renames (the module
 * keeps its name): AT+TM, then AT+CN00, AT+B501, AT+B401, 150 ms apart. */
static const char *const kInit[] = {"AT+TM", "AT+CN00", "AT+B501", "AT+B401"};

void bt_task(uint32_t now_ms)
{
    size_t avail = LPUART_TransferGetRxRingBufferLength(LPUART5, &handle);
    if (avail) {
        uint8_t buf[64];
        size_t got = 0;
        if (avail > sizeof buf) avail = sizeof buf;
        lpuart_transfer_t x = {.data = buf, .dataSize = avail};
        if (LPUART_TransferReceiveNonBlocking(LPUART5, &handle, &x, &got) == kStatus_Success) {
            got = avail;   /* ring had the data: copied synchronously */
            rx_total += got;
            for (size_t i = 0; i < got; i++) {
                if (log_len < sizeof log_buf) log_buf[log_len++] = buf[i];
                else { memmove(log_buf, log_buf + 1, sizeof log_buf - 1); log_buf[sizeof log_buf - 1] = buf[i]; }
            }
            bt_rx_frame_bytes(buf, got);
        }
    }
    if (step >= 0 && step < (int)(sizeof kInit / sizeof kInit[0]) && (int32_t)(now_ms - next_ms) >= 0) {
        if (next_ms == 0) { next_ms = now_ms + 500u; return; }   /* let the module boot */
        if (bt_at(kInit[step]) == 0) { step++; next_ms = now_ms + 150u; }
    }
}

void bt_status(void)
{
    log_printf("bt: init step %d/4, rx %lu bytes, tx %s; last rx:\r\n  ", step,
               (unsigned long)rx_total, tx_busy ? "busy" : "idle");
    for (size_t i = 0; i < log_len; i++) {
        uint8_t c = log_buf[i];
        if (c >= 0x20 && c < 0x7F) log_printf("%c", c);
        else if (c == '\n') log_printf("\\n");
        else if (c == '\r') log_printf("\\r");
        else log_printf("<%02x>", c);
    }
    log_printf("\r\n");
}
