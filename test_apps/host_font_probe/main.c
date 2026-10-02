/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ask the real LVGL whether the generated fonts can supply glyphs.
 *
 * This is the check that "it compiles" and "the render looks plausible" cannot
 * make: a font whose glyph_dsc[] stride does not match LVGL's
 * lv_font_fmt_txt_glyph_dsc_t loads fine, reports a sane line_height, and then
 * produces an empty bitmap for every character. That is what
 * LV_FONT_FMT_TXT_LARGE being out of step with the generator looks like.
 *
 *   cmake -S test_apps/host_font_probe -B build/host_font_probe -G Ninja
 *   cmake --build build/host_font_probe
 *   ./build/host_font_probe/host_font_probe
 */

#include <stdio.h>
#include <stddef.h>

#include "lvgl.h"

/* Defined by the generated files, which this target compiles in. */
extern const lv_font_t lv_font_ime_20;
extern const lv_font_t lv_font_ime_16;
/* LVGL's own built-in font: the reference for what the layout must look like. */
extern const lv_font_t lv_font_montserrat_14;

static int probe(const lv_font_t *font, const char *name, uint32_t letter_cp, const char *what)
{
    lv_font_glyph_dsc_t dsc;
    lv_memzero(&dsc, sizeof(dsc));

    bool found = lv_font_get_glyph_dsc(font, &dsc, letter_cp, 0);
    if (!found) {
        printf("  %-6s U+%04X %-8s NOT FOUND\n", name, (unsigned)letter_cp, what);
        return 0;
    }

    /*
     * lv_font_get_glyph_bitmap() wants a caller provided draw buffer for fonts
     * that need decoding; passing NULL makes it dereference NULL. The generated
     * fonts keep their bitmaps in flash, so ask for the static bitmap instead -
     * that is the path the renderer uses for them.
     */
    const void *bitmap = lv_font_get_glyph_static_bitmap(&dsc);

    printf("  %-6s U+%04X %-8s box=%ux%u adv=%d ofs=(%d,%d) stride=%u format=%u bitmap=%s\n",
           name, (unsigned)letter_cp, what,
           (unsigned)dsc.box_w, (unsigned)dsc.box_h, (int)dsc.adv_w,
           (int)dsc.ofs_x, (int)dsc.ofs_y, (unsigned)dsc.stride, (unsigned)dsc.format,
           bitmap != NULL ? "yes" : "NULL");

    if (dsc.entry != NULL && font->release_glyph != NULL) {
        font->release_glyph(font, &dsc);
    }

    /* A found glyph whose box is empty, or no bitmap, is the failure mode. */
    return (dsc.box_w > 0 && dsc.box_h > 0 && bitmap != NULL) ? 1 : 0;
}

static int check_font(const lv_font_t *font, const char *name)
{
    const lv_font_fmt_txt_dsc_t *fdsc = (const lv_font_fmt_txt_dsc_t *)font->dsc;
    printf("%s: line_height=%d base_line=%d dsc_bpp=%d bitmap_format=%d stride=%d "
           "dyn_loaded=%d cmap_num=%d kern_classes=%d kern_dsc=%p glyph_dsc=%p\n",
           name, (int)font->line_height, (int)font->base_line, (int)fdsc->bpp,
           (int)fdsc->bitmap_format, (int)fdsc->stride,
           (int)fdsc->are_glyphs_dynamic_loaded, (int)fdsc->cmap_num,
           (int)fdsc->kern_classes, (const void *)fdsc->kern_dsc,
           (const void *)fdsc->glyph_dsc);
    fflush(stdout);

    int good = 0;
    int total = 0;
    total++; good += probe(font, name, 'q', "latin");
    total++; good += probe(font, name, 'A', "latin");
    total++; good += probe(font, name, 0x4F60, "CJK");     /* 你 */
    total++; good += probe(font, name, 0x7B26, "CJK");     /* 符 */
    /* U+2190 is the backspace caption. U+232B (⌫) is intentionally not used:
     * DreamHanSansSC-W17 has no glyph for it, so the keyboard draws ←. */
    total++; good += probe(font, name, 0x2190, "symbol");
    printf("  -> %d/%d glyphs usable\n\n", good, total);
    fflush(stdout);
    return good;
}

int main(void)
{
    lv_init();

    printf("LV_FONT_FMT_TXT_LARGE = %d\n", (int)LV_FONT_FMT_TXT_LARGE);
    printf("sizeof(lv_font_t)=%u sizeof(lv_font_fmt_txt_dsc_t)=%u sizeof(glyph_dsc)=%u\n",
           (unsigned)sizeof(lv_font_t), (unsigned)sizeof(lv_font_fmt_txt_dsc_t),
           (unsigned)sizeof(lv_font_fmt_txt_glyph_dsc_t));
    printf("lv_font_t offsets: dsc=%u fallback=%u user_data=%u get_glyph_dsc=%u "
           "get_glyph_bitmap=%u release_glyph=%u line_height=%u base_line=%u\n\n",
           (unsigned)offsetof(lv_font_t, dsc), (unsigned)offsetof(lv_font_t, fallback),
           (unsigned)offsetof(lv_font_t, user_data),
           (unsigned)offsetof(lv_font_t, get_glyph_dsc),
           (unsigned)offsetof(lv_font_t, get_glyph_bitmap),
           (unsigned)offsetof(lv_font_t, release_glyph),
           (unsigned)offsetof(lv_font_t, line_height),
           (unsigned)offsetof(lv_font_t, base_line));

    printf("built-in montserrat_14: dsc=%p get_glyph_dsc=%p line_height=%d\n",
           (const void *)lv_font_montserrat_14.dsc,
           (const void *)(uintptr_t)lv_font_montserrat_14.get_glyph_dsc,
           (int)lv_font_montserrat_14.line_height);
    printf("generated  ime_20     : dsc=%p get_glyph_dsc=%p line_height=%d\n\n",
           (const void *)lv_font_ime_20.dsc,
           (const void *)(uintptr_t)lv_font_ime_20.get_glyph_dsc,
           (int)lv_font_ime_20.line_height);

    int good = 0;
    good += check_font(&lv_font_ime_20, "20px");
    good += check_font(&lv_font_ime_16, "16px");

    if (good == 0) {
        printf("RESULT: the generated fonts supply NO usable glyphs.\n"
               "        Check that LV_FONT_FMT_TXT_LARGE matches the generator\n"
               "        (lv_font_conv emits the compact 8 byte descriptor, so it must be 0).\n");
        return 1;
    }
    printf("RESULT: %d glyph probes usable\n", good);
    return 0;
}
