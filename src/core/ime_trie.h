/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Index over the T9 syllable table. See ime_trie.c for why this is a scan and
 * not a node table.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One syllable that matches a digit prefix. */
typedef struct {
    const char *syllable; /**< full pinyin, for example "xiang" */
    const char *digits;   /**< its key sequence, for example "94264" */
    bool is_exact;        /**< true when the digits match the prefix exactly */
} ime_trie_match_t;

/** Prepare the index. Idempotent. */
bool ime_trie_init(void);

/** Release the index. */
void ime_trie_deinit(void);

/** True once ime_trie_init() ran. */
bool ime_trie_is_ready(void);

/** Number of syllables in the table. */
size_t ime_trie_syllable_count(void);

/** Syllable string at an absolute table index, or NULL. */
const char *ime_trie_syllable(size_t index);

/** Digit string of the syllable at an absolute table index, or NULL. */
const char *ime_trie_syllable_digits(size_t index);

/** True when @p prefix can still be extended into a syllable. */
bool ime_trie_is_prefix(const char *prefix);

/**
 * Syllables that start with @p prefix, shortest spelling first.
 *
 * @param out  caller owned array
 * @param max  capacity of @p out
 * @return number of matches written
 */
size_t ime_trie_children(const char *prefix, ime_trie_match_t *out, size_t max);

/**
 * Find the syllable whose key sequence is exactly @p digits.
 *
 * @param out_index  receives the table index
 */
bool ime_trie_exact(const char *digits, size_t *out_index);

#ifdef __cplusplus
}
#endif
