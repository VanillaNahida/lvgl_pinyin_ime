/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Index over the T9 syllable table.
 *
 * The 9-key front end repeatedly asks two questions while the user types:
 *
 *   - "which syllables can 94 still grow into?"  -> ime_trie_children()
 *   - "is 946 a prefix of any syllable?"         -> ime_trie_is_prefix()
 *
 * Both are answered by a linear walk over the generated table
 * (src/generated/ime_t9_syllables.h). The table holds 413 entries of at most
 * five digits, so a full walk is a few microseconds on the ESP32-S3 and never
 * needs a node table, allocation-free after init, or a code-generated trie.
 * The scan order is stabilised by sorting the matches by syllable length, which
 * is the order the 9-key candidate row wants.
 *
 * The module is named "trie" because that is its place in the architecture
 * (docs/architecture.md); the implementation is the smallest thing that
 * answers the queries correctly.
 */

#include "ime_trie.h"

#include "generated/ime_t9_syllables.h"

#include <string.h>

static bool s_inited;

bool ime_trie_init(void)
{
    s_inited = true;
    return true;
}

void ime_trie_deinit(void)
{
    s_inited = false;
}

bool ime_trie_is_ready(void)
{
    return s_inited;
}

size_t ime_trie_syllable_count(void)
{
    return IME_T9_SYLLABLE_COUNT;
}

const char *ime_trie_syllable(size_t index)
{
    if (index >= IME_T9_SYLLABLE_COUNT) {
        return NULL;
    }
    return ime_t9_syllables[index].syllable;
}

const char *ime_trie_syllable_digits(size_t index)
{
    if (index >= IME_T9_SYLLABLE_COUNT) {
        return NULL;
    }
    return ime_t9_syllables[index].digits;
}

bool ime_trie_is_prefix(const char *prefix)
{
    if (prefix == NULL || prefix[0] == '\0') {
        return false;
    }

    size_t len = strlen(prefix);
    for (size_t i = 0; i < IME_T9_SYLLABLE_COUNT; i++) {
        const char *digits = ime_t9_syllables[i].digits;
        if (strlen(digits) >= len && strncmp(digits, prefix, len) == 0) {
            return true;
        }
    }
    return false;
}

size_t ime_trie_children(const char *prefix, ime_trie_match_t *out, size_t max)
{
    if (prefix == NULL || prefix[0] == '\0' || out == NULL || max == 0) {
        return 0;
    }

    size_t len = strlen(prefix);
    size_t count = 0;

    for (size_t i = 0; i < IME_T9_SYLLABLE_COUNT && count < max; i++) {
        const char *digits = ime_t9_syllables[i].digits;
        if (strlen(digits) < len || strncmp(digits, prefix, len) != 0) {
            continue;
        }

        out[count].syllable = ime_t9_syllables[i].syllable;
        out[count].digits = digits;
        out[count].is_exact = (strlen(digits) == len);
        count++;
    }

    /* Shortest spelling first: it is both the most likely and the most compact
     * to show in the 9-key row. Insertion sort, at most a handful of entries. */
    for (size_t i = 1; i < count; i++) {
        ime_trie_match_t item = out[i];
        size_t j = i;
        while (j > 0 && strlen(out[j - 1].syllable) > strlen(item.syllable)) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = item;
    }

    return count;
}

bool ime_trie_exact(const char *digits, size_t *out_index)
{
    if (digits == NULL || digits[0] == '\0') {
        return false;
    }

    size_t len = strlen(digits);
    for (size_t i = 0; i < IME_T9_SYLLABLE_COUNT; i++) {
        const char *d = ime_t9_syllables[i].digits;
        if (strlen(d) == len && strcmp(d, digits) == 0) {
            if (out_index != NULL) {
                *out_index = i;
            }
            return true;
        }
    }
    return false;
}
