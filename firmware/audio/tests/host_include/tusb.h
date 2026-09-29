/* Host stub (tests/fuzz_host_test.c): the CDC calls console.c makes. */
#ifndef FB200_HOST_TUSB_H
#define FB200_HOST_TUSB_H
#include <stdbool.h>
#include <stdint.h>
uint32_t tud_cdc_available(void);
int32_t tud_cdc_read_char(void);
#endif
