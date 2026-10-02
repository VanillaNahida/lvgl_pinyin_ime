/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Patch P9: width-explicit dictionary serialisation. See include/dictio.h.
 */

#include "../include/dictio.h"

#include <string.h>

bool dict_write_word(FILE *fp, size_t value)
{
    uint8_t buf[DICT_WORD_BYTES];
    for (int i = 0; i < DICT_WORD_BYTES; i++) {
        buf[i] = (uint8_t)((value >> (8 * i)) & 0xFFu);
    }
    return fwrite(buf, 1, sizeof(buf), fp) == sizeof(buf);
}

bool dict_read_word(FILE *fp, size_t *value)
{
    uint8_t buf[DICT_WORD_BYTES];
    if (fread(buf, 1, sizeof(buf), fp) != sizeof(buf)) {
        return false;
    }

    size_t out = 0;
    for (int i = 0; i < DICT_WORD_BYTES; i++) {
        out |= (size_t)buf[i] << (8 * i);
    }
    if (value != NULL) {
        *value = out;
    }
    return true;
}

bool dict_write_words(FILE *fp, const size_t *values, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (!dict_write_word(fp, values[i])) {
            return false;
        }
    }
    return true;
}

bool dict_read_words(FILE *fp, size_t *values, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (!dict_read_word(fp, &values[i])) {
            return false;
        }
    }
    return true;
}
