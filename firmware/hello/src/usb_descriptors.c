/* Placeholder descriptor callbacks so the TinyUSB device stack links before
 * Task 6 adds the real CDC descriptors. They are weak, so Task 6 replaces
 * this file with strong definitions without any duplicate-symbol risk. */
#include "tusb.h"

TU_ATTR_WEAK uint8_t const *tud_descriptor_device_cb(void)
{
    return NULL;
}

TU_ATTR_WEAK uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return NULL;
}

TU_ATTR_WEAK uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)index;
    (void)langid;
    return NULL;
}
