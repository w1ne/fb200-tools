/* Copyright (C) 2026 Andrii Shylenko
 *
 * This software is released under the MIT License.
 * See the LICENSE file in the project root for full license information.
 */

#ifndef FB200_LOG_FMT_H
#define FB200_LOG_FMT_H
#include <stdarg.h>
#include <stddef.h>
/* Minimal printf subset (%s %c %d %u %x %% with optional zero/space pad).
 * Returns the number of characters that would be written; the output is
 * clamped to cap. Pure and host-testable. */
size_t log_vformat(char *dst, size_t cap, const char *fmt, va_list ap);
#endif
