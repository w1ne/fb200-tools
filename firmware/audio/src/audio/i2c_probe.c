/* Phase 0 RE tool: scan the LPI2C buses and dump a codec's register file.
 * See docs/superpowers/plans/2026-09-27-fb200-audio-engine.md (Task 4). */
#include "fsl_clock.h"
#include "fsl_iomuxc.h"
#include "fsl_lpi2c.h"
#include "debug/cdc_log.h"
#include "i2c_probe.h"

static LPI2C_Type *const kBuses[4] = { LPI2C1, LPI2C2, LPI2C3, LPI2C4 };

/* Bounded wait for any of the given status flags. */
static bool wait_flags(LPI2C_Type *base, uint32_t flags, uint32_t loops)
{
    while (loops--) {
        if (LPI2C_MasterGetStatusFlags(base) & flags) return true;
    }
    return false;
}

/* Address probe without data: START + address, check NACK, STOP. The SDK's
 * zero-length blocking transfer never completes, so use the primitives. */
static bool probe_addr(LPI2C_Type *base, uint8_t addr)
{
    LPI2C_MasterClearStatusFlags(base, kLPI2C_MasterClearFlags);
    if (LPI2C_MasterStart(base, addr, kLPI2C_Write) != kStatus_Success) return false;
    if (!wait_flags(base, kLPI2C_MasterEndOfPacketFlag | kLPI2C_MasterNackDetectFlag, 200000u)) {
        LPI2C_MasterStop(base);
        return false;
    }
    bool acked = (LPI2C_MasterGetStatusFlags(base) & kLPI2C_MasterNackDetectFlag) == 0u;
    LPI2C_MasterStop(base);
    (void)wait_flags(base, kLPI2C_MasterStopDetectFlag, 200000u);
    return acked;
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
                log_printf(" %02x", a);
                found++;
            }
        }
        if (!found) log_printf(" (none)");
        log_printf("\r\n");
    }
}

void i2c_dump(uint8_t bus, uint8_t addr)
{
    if (bus < 1 || bus > 4) { log_printf("bus must be 1..4\r\n"); return; }
    LPI2C_Type *base = kBuses[bus - 1];
    log_printf("dump bus%d addr %02x\r\n", bus, addr);
    for (uint8_t reg = 0x00; reg < 0x80; reg++) {
        uint8_t v = 0;
        lpi2c_master_transfer_t t = {
            .slaveAddress = addr, .direction = kLPI2C_Write, .data = &reg, .dataSize = 1,
            .flags = kLPI2C_TransferNoStopFlag,
        };
        if (LPI2C_MasterTransferBlocking(base, &t) != kStatus_Success) {
            log_printf("%02x: ERR\r\n", reg);
            continue;
        }
        t = (lpi2c_master_transfer_t){
            .slaveAddress = addr, .direction = kLPI2C_Read, .data = &v, .dataSize = 1,
        };
        if (LPI2C_MasterTransferBlocking(base, &t) == kStatus_Success) log_printf("%02x: %02x\r\n", reg, v);
        else log_printf("%02x: ERR\r\n", reg);
    }
}
