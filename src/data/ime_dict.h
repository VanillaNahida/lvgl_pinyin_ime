/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dictionary loading and the read-only VFS that exposes the flash partition as
 * a file, because libgooglepinyin opens its system dictionary with fopen().
 *
 * Three sources are supported, selected by Kconfig:
 *   - PARTITION: mmap() the "dict" partition and serve it as /dict/dict_pinyin.dat
 *   - SDCARD:    use the file already on the mounted SD card
 *   - EMBEDDED:  copy the dictionary blob linked into the application into PSRAM
 *                and serve it under the same path
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Make the system dictionary available at @p out_path (at least 32 bytes).
 *
 * @return true when the path can be fopen()ed by the engine
 */
bool ime_dict_prepare(char *out_path, size_t out_path_size);

/** Release whatever ime_dict_prepare() mapped or copied. */
void ime_dict_release(void);

/** Size of the dictionary that was made available, or 0. */
size_t ime_dict_size(void);

/**
 * Re-open the prepared dictionary and read it end to end.
 *
 * Written for bring-up: it separates "the engine refused the dictionary" from
 * "the file cannot be read at all", and reports errno when the open fails.
 */
bool ime_dict_self_test(void);

/** Path of the user dictionary file, or NULL when it must stay in RAM only. */
const char *ime_dict_user_path(void);

#ifdef __cplusplus
}
#endif
