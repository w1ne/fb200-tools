/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_I2C_PROBE_H
#define FB200_I2C_PROBE_H
#include <stdint.h>
void i2c_probe_init(void);
void i2c_scan_all(void);
void i2c_dump(uint8_t bus, uint8_t addr);
void i2c_dump_found(void);
#endif
