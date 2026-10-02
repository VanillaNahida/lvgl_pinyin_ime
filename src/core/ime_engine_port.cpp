/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The only translation unit that talks to libgooglepinyin.
 *
 * libgooglepinyin exports its API under C++ linkage from namespace ime_pinyin,
 * so wrapping it here means the rest of the component (and the host unit tests)
 * can stay plain C.
 */

#include "lvgl_pinyin_ime/ime_engine.h"

#include "dictdef.h"
#include "pinyinime.h"

#include <stdlib.h>
#include <string.h>

using namespace ime_pinyin;

namespace {

/* pinyin is ASCII: the engine wants a plain char buffer of syllables
 * ('\'' separators included), not a UTF-16 one. */
const size_t kPinyinMax = IME_ENGINE_MAX_PINYIN_LEN;
char s_pinyin8[kPinyinMax + 1];

/* Shared scratch for candidate / prediction conversion. 256 UTF-16 code units
 * is far more than a full sentence candidate needs. */
const size_t kTextMax16 = 256;

/* UTF-16 (BMP) to UTF-8. Returns the byte length, or 0 on failure. */
size_t utf16_to_utf8(const char16 *src, char *dst, size_t dst_size)
{
    if (src == NULL || dst == NULL || dst_size == 0) {
        return 0;
    }

    size_t o = 0;
    for (size_t i = 0; src[i] != 0; i++) {
        uint32_t cp = src[i];
        size_t need = cp < 0x80 ? 1 : (cp < 0x800 ? 2 : 3);
        if (o + need >= dst_size) {
            break;
        }
        if (need == 1) {
            dst[o++] = (char)cp;
        } else if (need == 2) {
            dst[o++] = (char)(0xC0 | (cp >> 6));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            dst[o++] = (char)(0xE0 | (cp >> 12));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    dst[o] = '\0';
    return o;
}

/* UTF-8 to UTF-16 (BMP). Returns the code unit count, 0 on failure. */
size_t utf8_to_utf16(const char *src, char16 *dst, size_t dst_size)
{
    if (src == NULL || dst == NULL || dst_size == 0) {
        return 0;
    }

    size_t i = 0;
    size_t o = 0;
    while (src[i] != '\0') {
        uint32_t cp = (unsigned char)src[i];
        if (cp < 0x80) {
            i += 1;
        } else if ((cp & 0xE0) == 0xC0) {
            cp = ((cp & 0x1F) << 6) | ((unsigned char)src[i + 1] & 0x3F);
            i += 2;
        } else if ((cp & 0xF0) == 0xE0) {
            cp = ((cp & 0x0F) << 12) | (((unsigned char)src[i + 1] & 0x3F) << 6) |
                 ((unsigned char)src[i + 2] & 0x3F);
            i += 3;
        } else {
            /* Outside the BMP: skip one byte and resync. */
            i += 1;
            continue;
        }
        if (o + 1 >= dst_size) {
            break;
        }
        dst[o++] = (char16)cp;
    }
    dst[o] = 0;
    return o;
}

size_t s_predict_count = 0;
char16(*s_predict_items)[kMaxPredictSize + 1] = NULL;

} // namespace

bool ime_engine_open(const char *sys_dict_path, const char *usr_dict_path)
{
    /* MatrixSearch::init() rejects a NULL user dictionary path even though it
     * tolerates a file that does not exist, so an empty path is the RAM-only
     * configuration. */
    static const char kNoUserDict[] = "";
    const char *user = (usr_dict_path != NULL && usr_dict_path[0] != '\0') ? usr_dict_path
                                                                           : kNoUserDict;
    if (sys_dict_path == NULL) {
        return false;
    }
    return im_open_decoder(sys_dict_path, user);
}

void ime_engine_close(void)
{
    im_close_decoder();
    s_predict_items = NULL;
    s_predict_count = 0;
}

void ime_engine_set_max_lens(size_t max_pinyin_len, size_t max_hz_len)
{
    im_set_max_lens(max_pinyin_len, max_hz_len);
}

size_t ime_engine_search(const char *pinyin_utf8, size_t len)
{
    if (pinyin_utf8 == NULL || len > kPinyinMax) {
        return 0;
    }

    /* The engine keeps its own copy of the spelling string and does an
     * incremental search against the previous one, so an incremental caller
     * (one letter at a time) is correct and cheap; ime_engine_reset() starts a
     * fresh search. */
    memcpy(s_pinyin8, pinyin_utf8, len);
    s_pinyin8[len] = '\0';

    return im_search(s_pinyin8, len);
}

size_t ime_engine_candidate(size_t index, char *out_utf8, size_t out_size)
{
    char16 buf[kTextMax16];
    char16 *got = im_get_candidate(index, buf, kTextMax16);
    if (got == NULL) {
        return 0;
    }
    if (out_utf8 == NULL) {
        return (size_t)utf16_strlen(buf);
    }
    return utf16_to_utf8(buf, out_utf8, out_size);
}

size_t ime_engine_choose(size_t index)
{
    return im_choose(index);
}

size_t ime_engine_fixed_len(void)
{
    return im_get_fixed_len();
}

const char *ime_engine_remaining_pinyin(size_t *out_len)
{
    size_t decoded_len = 0;
    const char *pys = im_get_sps_str(&decoded_len);

    /*
     * The engine keeps the whole input and records how much of it is already
     * fixed, so the still-live part starts after the fixed spellings rather than
     * at the front. get_spl_start() gives those boundaries: element [n] is the
     * start of the n-th spelling, and the array has one extra entry holding the
     * end, so [fixed] is exactly the offset of the first unfixed syllable.
     */
    size_t fixed = im_get_fixed_len();
    if (pys == NULL) {
        if (out_len != NULL) {
            *out_len = 0;
        }
        return NULL;
    }

    const uint16 *spl_start = NULL;
    size_t spl_num = im_get_spl_start_pos(spl_start);
    if (spl_start == NULL || fixed > spl_num) {
        if (out_len != NULL) {
            *out_len = 0;
        }
        return NULL;
    }

    size_t offset = spl_start[fixed];
    if (offset > decoded_len) {
        offset = decoded_len;
    }

    if (out_len != NULL) {
        *out_len = decoded_len - offset;
    }
    return pys + offset;
}

size_t ime_engine_cancel_choice(void)
{
    return im_cancel_last_choice();
}

void ime_engine_reset(void)
{
    im_reset_search();
}

size_t ime_engine_predict(const char *history_utf8, char *out_utf8, size_t out_size)
{
    if (history_utf8 == NULL || history_utf8[0] == '\0') {
        return 0;
    }

    char16 his[kTextMax16];
    if (utf8_to_utf16(history_utf8, his, kTextMax16) == 0) {
        return 0;
    }

    s_predict_count = im_get_predicts(his, s_predict_items);
    if (s_predict_count == 0) {
        s_predict_items = NULL;
        return 0;
    }

    if (out_utf8 != NULL && out_size > 0) {
        utf16_to_utf8(s_predict_items[0], out_utf8, out_size);
    }
    return s_predict_count;
}

size_t ime_engine_predict_item(size_t index, char *out_utf8, size_t out_size)
{
    if (s_predict_items == NULL || index >= s_predict_count) {
        return 0;
    }
    if (out_utf8 == NULL) {
        return (size_t)utf16_strlen(s_predict_items[index]);
    }
    return utf16_to_utf8(s_predict_items[index], out_utf8, out_size);
}

size_t ime_engine_spl_start_pos(const uint16_t **positions)
{
    const uint16 *ref = NULL;
    size_t n = im_get_spl_start_pos(ref);
    if (positions != NULL) {
        *positions = (const uint16_t *)ref;
    }
    return n;
}

size_t ime_engine_utf8_to_utf16(const char *src, uint16_t *dst, size_t dst_len)
{
    return utf8_to_utf16(src, (char16 *)dst, dst_len);
}

size_t ime_engine_utf16_to_utf8(const uint16_t *src, char *dst, size_t dst_len)
{
    return utf16_to_utf8((const char16 *)src, dst, dst_len);
}
