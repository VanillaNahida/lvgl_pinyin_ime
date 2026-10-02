/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 9-key (T9) digit buffer to pinyin segmentation. No LVGL dependency.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef IME_T9_MAX_DIGITS
#define IME_T9_MAX_DIGITS 6
#endif

#ifndef IME_T9_MAX_PINYIN
#define IME_T9_MAX_PINYIN 20
#endif

#ifndef IME_T9_MAX_PINYIN_LEN
#define IME_T9_MAX_PINYIN_LEN 32
#endif

#ifndef IME_T9_PARTIAL_MAX
#define IME_T9_PARTIAL_MAX 6
#endif

/** Reset the state. Must be called once before use. */
void ime_t9_init(void);

/** Release the state. */
void ime_t9_deinit(void);

/** Drop the digit buffer and every segmentation. */
void ime_t9_reset(void);

/**
 * Append a digit.
 *
 * @return false when the digit is out of range, the buffer is full, or the
 *         resulting buffer cannot be read as pinyin (then nothing changes)
 */
bool ime_t9_push_digit(char digit);

/** Remove the last digit. */
void ime_t9_backspace(void);

/** Number of digits in the buffer. */
size_t ime_t9_len(void);

/** The digit buffer, never NULL. */
const char *ime_t9_digits(void);

/** Number of pinyin strings the current buffer can be segmented into. */
size_t ime_t9_count(void);

/** Pinyin string @p index, or NULL when out of range. */
const char *ime_t9_pinyin(size_t index);

/**
 * True when every syllable of @p pinyin is one the built-in syllable table knows.
 *
 * The table (src/generated/ime_t9_syllables.h, built from the engine's own word
 * list) is the project's "common pinyin" list: it is what the 9-key front end
 * segments against, so anything it rejects can never be a real spelling. A
 * trailing separator is allowed, because a partial suggestion is shown with one.
 *
 * Syllables are separated by '\''.
 */
bool ime_t9_is_valid(const char *pinyin);

/** Index of the currently highlighted pinyin string. */
size_t ime_t9_selected_index(void);

/** The highlighted pinyin string, or "" when nothing is buffered. */
const char *ime_t9_selected_pinyin(void);

/** Highlight pinyin string @p index. */
bool ime_t9_select(size_t index);

#ifdef __cplusplus
}
#endif
