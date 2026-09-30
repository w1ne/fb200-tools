/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#include <string.h>
#include "preset/preset.h"
#include "debug/selfupdate.h"

#define FLASH_AHB 0x60000000u

void preset_read(unsigned index, preset_t *out)
{
    memcpy(out, (const void *)(FLASH_AHB + PRESET_FLASH + (index % PRESET_COUNT) * PRESET_STRIDE),
           sizeof *out);
}

int preset_write(unsigned index, const preset_t *p)
{
    if (index >= PRESET_COUNT) return -1;
    return flash_store(PRESET_FLASH + index * PRESET_STRIDE, p, sizeof *p);
}

void settings_read(settings_t *out)
{
    memcpy(out, (const void *)(FLASH_AHB + SETTINGS_FLASH), sizeof *out);
}

int settings_write(const settings_t *s)
{
    return flash_store(SETTINGS_FLASH, s, sizeof *s);
}

void rhythm_settings_read(uint8_t out[RHYTHM_SIZE])
{
    memcpy(out, (const void *)(FLASH_AHB + RHYTHM_FLASH), RHYTHM_SIZE);
}

int rhythm_settings_write(const uint8_t in[RHYTHM_SIZE])
{
    return flash_store(RHYTHM_FLASH, in, RHYTHM_SIZE);
}
