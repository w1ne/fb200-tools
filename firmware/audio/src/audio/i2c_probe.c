/* Phase 0 RE tool: scan the LPI2C buses and dump a codec's register file. */
#include "fsl_clock.h"
#include "fsl_iomuxc.h"
#include "fsl_lpi2c.h"
#include "debug/cdc_log.h"
#include "i2c_probe.h"

static LPI2C_Type *const kBuses[4] = { LPI2C1, LPI2C2, LPI2C3, LPI2C4 };

/* First device found by the scan; 'd' dumps it. */
static uint8_t g_found_bus = 0;
static uint8_t g_found_addr = 0;

/* Address probe: one-byte read through the SDK transfer (bounded by
 * I2C_RETRY_TIMES). A present device ACKs the address; absent ones NACK and
 * the transfer returns kStatus_LPI2C_Nak. */
static bool probe_addr(LPI2C_Type *base, uint8_t addr)
{
    uint8_t v = 0;
    lpi2c_master_transfer_t t = {
        .slaveAddress = addr, .direction = kLPI2C_Read, .data = &v, .dataSize = 1,
    };
    return LPI2C_MasterTransferBlocking(base, &t) == kStatus_Success;
}

/* Read register `reg` (codec register-index protocol) and report the value. */
static bool read_reg(LPI2C_Type *base, uint8_t addr, uint8_t reg, uint8_t *out)
{
    lpi2c_master_transfer_t t = {
        .slaveAddress = addr, .direction = kLPI2C_Write, .data = &reg, .dataSize = 1,
        .flags = kLPI2C_TransferNoStopFlag,
    };
    if (LPI2C_MasterTransferBlocking(base, &t) != kStatus_Success) return false;
    t = (lpi2c_master_transfer_t){
        .slaveAddress = addr, .direction = kLPI2C_Read, .data = out, .dataSize = 1,
    };
    return LPI2C_MasterTransferBlocking(base, &t) == kStatus_Success;
}

/* I2C pad muxing recovered from the stock image (docs/AUDIO_PATH.md):
 * LPI2C1 SCL/SDA = GPIO_SD_B1_04/05, LPI2C3 = GPIO_EMC_21/22,
 * LPI2C4 = GPIO_EMC_11/12. Open-drain with pull-ups, slow slew. */
#define I2C_PAD_CFG 0xB808u

static void configure_i2c_pads(void)
{
    IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B1_04_LPI2C1_SCL, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_SD_B1_05_LPI2C1_SDA, 1U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_SD_B1_04_LPI2C1_SCL, I2C_PAD_CFG);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_SD_B1_05_LPI2C1_SDA, I2C_PAD_CFG);
    IOMUXC_SetPinMux(IOMUXC_GPIO_EMC_21_LPI2C3_SDA, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_EMC_22_LPI2C3_SCL, 1U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_EMC_21_LPI2C3_SDA, I2C_PAD_CFG);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_EMC_22_LPI2C3_SCL, I2C_PAD_CFG);
    IOMUXC_SetPinMux(IOMUXC_GPIO_EMC_11_LPI2C4_SDA, 1U);
    IOMUXC_SetPinMux(IOMUXC_GPIO_EMC_12_LPI2C4_SCL, 1U);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_EMC_11_LPI2C4_SDA, I2C_PAD_CFG);
    IOMUXC_SetPinConfig(IOMUXC_GPIO_EMC_12_LPI2C4_SCL, I2C_PAD_CFG);
}

void i2c_probe_init(void)
{
    uint32_t clk = CLOCK_GetClockRootFreq(kCLOCK_Lpi2cClkRoot);
    if (clk == 0u) clk = 24000000u;   /* fallback: OSC */
    log_printf("lpi2c clk=%u Hz\r\n", (unsigned)clk);
    configure_i2c_pads();
    for (int i = 0; i < 4; i++) {
        lpi2c_master_config_t cfg;
        LPI2C_MasterGetDefaultConfig(&cfg);
        cfg.baudRate_Hz = 100000u;
        cfg.busIdleTimeout_ns = 1000000u;    /* 1 ms */
        cfg.pinLowTimeout_ns = 10000000u;    /* 10 ms, aborts stuck buses */
        LPI2C_MasterInit(kBuses[i], &cfg, clk);
    }
    log_printf("lpi2c init done\r\n");
}

void i2c_scan_all(void)
{
    for (int bus = 0; bus < 4; bus++) {
        log_printf("bus%d:", bus + 1);
        int found = 0;
        for (uint8_t a = 0x08; a <= 0x77; a++) {
            if (probe_addr(kBuses[bus], a)) {
                uint8_t v = 0;
                bool readable = read_reg(kBuses[bus], a, 0x00, &v);
                log_printf(" %02x", a);
                if (readable) log_printf("[%02x]", v);
                if (!found) { g_found_bus = (uint8_t)(bus + 1); g_found_addr = a; }
                found++;
            }
        }
        if (!found) log_printf(" (none)");
        log_printf("\r\n");
    }
}

void i2c_dump_found(void)
{
    if (g_found_addr == 0) { log_printf("no device found yet\r\n"); return; }
    i2c_dump(g_found_bus, g_found_addr);
}

void i2c_dump(uint8_t bus, uint8_t addr)
{
    if (bus < 1 || bus > 4) { log_printf("bus must be 1..4\r\n"); return; }
    LPI2C_Type *base = kBuses[bus - 1];
    log_printf("dump bus%d addr %02x\r\n", bus, addr);
    for (uint8_t reg = 0x00; reg < 0x80; reg++) {
        uint8_t v = 0;
        if (read_reg(base, addr, reg, &v)) log_printf("%02x:%02x ", reg, v);
        else log_printf("%02x:ERR ", reg);
        if ((reg & 0x0F) == 0x0F) log_printf("\r\n");
    }
    log_printf("\r\n");
}
