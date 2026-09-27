#ifndef FB200_POWER_H
#define FB200_POWER_H
#include <stdbool.h>
#include <stdint.h>
/* Battery, charger and supply monitoring with the stock status LED
 * (docs/UI_AND_STORAGE.md §3), and the stock power-fail action: save the
 * settings and flip the B1_10/B1_15 latch pair when the supply sense drops. */
typedef struct {
    uint16_t battery_raw;   /* ADC1 IN9, AD_B1_04 */
    uint16_t supply_raw;    /* ADC1 IN7, AD_B1_02 (stock power-fail sense) */
    uint8_t level;          /* 0..4, stock thresholds */
    bool charging;          /* GPIO1_IO19, AD_B1_03 */
    bool supply_low;        /* supply_raw below the stock power-fail threshold */
} power_state_t;

void power_init(void);
void power_task(uint32_t now_ms);
const power_state_t *power_state(void);
void power_fail_changed(bool low);
#endif
