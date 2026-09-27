#ifndef FB200_CDC_LOG_H
#define FB200_CDC_LOG_H
#include <stddef.h>
void cdc_log_init(void);
void cdc_log_task(void);
void cdc_log_write(const char *data, size_t len);
void log_printf(const char *fmt, ...);
#endif
