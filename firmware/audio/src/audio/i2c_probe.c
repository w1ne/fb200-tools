/* Phase 0 RE tool: scan the LPI2C buses and dump a codec's register file.
 * See docs/superpowers/plans/2026-09-27-fb200-audio-engine.md (Task 4). */
#include "fsl_clock.h"
#include "fsl_lpi2c.h"
#include "debug/cdc_log.h"
#include "i2c_probe.h"

static LPI2C_Type *const kBuses[4] = { LPI2C1, LPI2C2, LPI2C3, LPI2C4 };

static bool probe_addr(LPI2C_Type *base, uint8_t addr)
{
    lpi2c_master_transfer_t t = {
        .slaveAddress = addr, .direction = kLPI2C_Write, .data = NULL, .dataSize = 0,
    };
    return LPI2C_MasterTransferBlocking(base, &t) == kStatus_Success;
}

void i2c_probe_init(void)
{
    uint32_t clk = CLOCK_GetClockRootFreq(kCLOCK_Lpi2cClkRoot);
    if (clk == 0u) clk = 24000000u;   /* fallback: OSC */
    log_printf("lpi2c clk=%u Hz\r\n", (unsigned)clk);
    for (int i = 0; i < 4; i++) {
        lpi2c_master_config_t cfg;
        LPI2C_MasterGetDefaultConfig(&cfg);
        cfg.baudRate_Hz = 100000u;
        LPI2C_MasterInit(kBuses[i], &cfg, clk);
    }
}

void i2c_scan_all(void)
{
    for (int bus = 0; bus < 4; bus++) {
        log_printf("bus%d:", bus + 1);
        for (uint8_t a = 0x08; a <= 0x77; a++) {
            if (probe_addr(kBuses[bus], a)) log_printf(" %02x", a);
        }
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
