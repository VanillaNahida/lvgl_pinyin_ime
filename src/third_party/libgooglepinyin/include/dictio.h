/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Patch P9 (lvgl_pinyin_ime): make the dictionary serialisation width-explicit.
 *
 * Upstream writes and reads its dictionary with `sizeof(size_t)` words:
 *
 *     fwrite(&scis_num_, sizeof(size_t), 1, fp);
 *
 * On a 64-bit host (which is where tools/gen_dict.py runs the model builder)
 * that emits 8-byte words, while the ESP32 build reads 4-byte words. The engine
 * then parses garbage and reports only "cannot open the dictionary".
 *
 * These two helpers define the on-disk word as 4 bytes on every platform, so
 * the host toolchain and the target always agree. They are used by both the
 * encoder and the decoder, so the format is also independent of the word size
 * of whoever reads it.
 *
 * Storage note: this is 1 071 858 bytes for the upstream word list either way
 * (the counts are small); the widest field here is a 32-bit index.
 */

#pragma once

#include <stdbool.h>
#include <stdio.h>

#include <stdint.h>

/** On-disk width of every serialised counter. Never change it. */
#define DICT_WORD_BYTES 4

/*
 * Patch P10: optional trace of the sizes the loader reads. Enable by defining
 * DICT_TRACE_ENABLE in the build; off by default so the firmware stays quiet.
 */
#ifdef DICT_TRACE_ENABLE
#include <stdio.h>
#define DICT_TRACE(...)                       \
    do {                                      \
        printf("[dict] ");                    \
        printf(__VA_ARGS__);                  \
        printf("\n");                         \
    } while (0)
#else
#define DICT_TRACE(...) ((void)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Write one counter as a 4-byte little-endian style native word. */
bool dict_write_word(FILE *fp, size_t value);

/** Read one counter back, widening it to size_t. */
bool dict_read_word(FILE *fp, size_t *value);

/** Write @p count counters (used for the start_pos_ / start_id_ arrays). */
bool dict_write_words(FILE *fp, const size_t *values, size_t count);

/** Read @p count counters. */
bool dict_read_words(FILE *fp, size_t *values, size_t count);

#ifdef __cplusplus
}
#endif
