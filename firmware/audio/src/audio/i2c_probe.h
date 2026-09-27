#ifndef FB200_I2C_PROBE_H
#define FB200_I2C_PROBE_H
#include <stdint.h>
void i2c_probe_init(void);
void i2c_scan_all(void);
void i2c_dump(uint8_t bus, uint8_t addr);
#endif
