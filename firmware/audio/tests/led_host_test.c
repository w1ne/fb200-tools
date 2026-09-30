/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* Host test: the LED heartbeat/streaming pattern. */
#include <assert.h>
#include <stdio.h>
#include "led.h"

int main(void)
{
    /* Streaming: always on. */
    assert(led_pattern(true, 0));
    assert(led_pattern(true, 12345));

    /* Idle: 1 Hz, 50% duty. */
    assert(led_pattern(false, 0));
    assert(led_pattern(false, 499));
    assert(!led_pattern(false, 500));
    assert(!led_pattern(false, 999));
    assert(led_pattern(false, 1000));

    printf("led host tests OK\n");
    return 0;
}
