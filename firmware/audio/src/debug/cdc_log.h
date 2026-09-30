/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_CDC_LOG_H
#define FB200_CDC_LOG_H
#include <stddef.h>
#include <stdint.h>
void cdc_log_init(void);
void cdc_log_task(void);
void cdc_log_write(const char *data, size_t len);
void log_printf(const char *fmt, ...);
void log_puts(const char *s);          /* no length limit (log_printf: 191) */
void log_flush_ms(uint32_t ms);        /* drain to the host before a reset */
#endif
