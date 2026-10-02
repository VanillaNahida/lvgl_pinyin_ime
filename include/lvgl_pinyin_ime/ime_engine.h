/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Engine facade. This header must stay free of any LVGL dependency so that the
 * engine can be built and unit tested on the host (test_apps/host_core_test).
 *
 * The implementation lives in ime_engine_port.cpp, the only file in the
 * component that includes the vendored libgooglepinyin headers.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest accepted pinyin buffer, including '\'' separators. */
#ifndef IME_ENGINE_MAX_PINYIN_LEN
#define IME_ENGINE_MAX_PINYIN_LEN 64
#endif

/** Longest accepted encoded result (a full sentence candidate). */
#define IME_ENGINE_MAX_TEXT_LEN 64

/**
 * Open the decoder.
 *
 * @param sys_dict_path  system dictionary (dict_pinyin.dat) path, must not be
 *                       NULL
 * @param usr_dict_path  user dictionary path, or NULL/"" to keep the user
 *                       dictionary in RAM only
 * @return true on success
 */
bool ime_engine_open(const char *sys_dict_path, const char *usr_dict_path);

/** Close the decoder and release engine memory. */
void ime_engine_close(void);

/** Set the maximum pinyin and result lengths (0 keeps the current value). */
void ime_engine_set_max_lens(size_t max_pinyin_len, size_t max_hz_len);

/**
 * Search candidates for a pinyin buffer.
 *
 * @param pinyin_utf8  pinyin string, syllables separated by '\''
 * @param len          byte length
 * @return number of candidates available
 */
size_t ime_engine_search(const char *pinyin_utf8, size_t len);

/**
 * Copy one candidate as UTF-8.
 *
 * @param index     candidate index, 0 based
 * @param out_utf8  destination buffer, or NULL to query the UTF-16 length
 * @param out_size  destination capacity in bytes
 * @return byte length written, 0 when the index is out of range
 */
size_t ime_engine_candidate(size_t index, char *out_utf8, size_t out_size);

/** Fix the candidate at @p index and return the remaining candidate count. */
size_t ime_engine_choose(size_t index);

/** Number of already fixed Chinese characters. */
size_t ime_engine_fixed_len(void);

/**
 * The pinyin the engine still holds, after the syllables that have already been
 * consumed by ime_engine_choose().
 *
 * This is what makes partial commits work: choosing 拉 out of "lawanle" leaves
 * the engine with "wanle", so the caller can keep composing instead of throwing
 * the rest of the input away. Valid until the next search/choose/reset.
 *
 * @param out_len  receives the decoded length in bytes
 * @return NUL terminated pinyin, or NULL when there is none
 */
const char *ime_engine_remaining_pinyin(size_t *out_len);

/** Undo the last ime_engine_choose(). */
size_t ime_engine_cancel_choice(void);

/** Drop the current search state. */
void ime_engine_reset(void);

/**
 * Ask the engine for association candidates following @p history_utf8.
 *
 * @param out_utf8  optional destination that receives the first prediction
 * @return total number of predictions; read them with ime_engine_predict_item()
 */
size_t ime_engine_predict(const char *history_utf8, char *out_utf8, size_t out_size);

/** Copy prediction number @p index as UTF-8, as returned by ime_engine_predict(). */
size_t ime_engine_predict_item(size_t index, char *out_utf8, size_t out_size);

/**
 * Segment boundaries of the last search.
 *
 * @param positions  receives a pointer to n + 1 pinyin string offsets, or NULL
 * @return number of syllables in the current segmentation
 */
size_t ime_engine_spl_start_pos(const uint16_t **positions);

/** UTF-8 to UTF-16LE (BMP only). Returns code units written. */
size_t ime_engine_utf8_to_utf16(const char *src, uint16_t *dst, size_t dst_len);

/** UTF-16LE (BMP only) to UTF-8. Returns bytes written. */
size_t ime_engine_utf16_to_utf8(const uint16_t *src, char *dst, size_t dst_len);

#ifdef __cplusplus
}
#endif
