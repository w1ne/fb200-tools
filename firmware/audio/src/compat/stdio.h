/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

/* Minimal freestanding <stdio.h>; the image does no stdio. TinyUSB's
 * board_api.h includes this header unconditionally. */
#pragma once

#include <stddef.h>
