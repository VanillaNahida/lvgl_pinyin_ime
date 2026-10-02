/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * PSRAM-first allocator (patch P1).
 *
 * libgooglepinyin loads its dictionary with malloc() and parses with new[].
 * On ESP32-S3 with PSRAM the dictionary tables (~1.3 MB) belong in external
 * RAM so that internal RAM stays free for the LVGL draw buffers, which the SPI
 * DMA cannot read from PSRAM.
 *
 * On the target the component links with
 *   -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc
 * so every upstream allocation lands here without editing upstream sources,
 * and new[]/delete (which call malloc/free) follow automatically.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** malloc() preferring PSRAM, falling back to internal RAM. */
void *ime_malloc(size_t size);

/** calloc() preferring PSRAM, falling back to internal RAM. */
void *ime_calloc(size_t n, size_t size);

/** realloc() that keeps the block on the heap it came from. */
void *ime_realloc(void *ptr, size_t size);

/** free() for pointers returned by the three functions above (and by malloc). */
void ime_free(void *ptr);

/** Bytes currently held by ime_malloc/ime_calloc/realloc, for diagnostics. */
size_t ime_alloc_used_bytes(void);

/** Number of live allocations, for diagnostics and leak assertions in tests. */
size_t ime_alloc_live_blocks(void);

#ifdef __cplusplus
}
#endif
