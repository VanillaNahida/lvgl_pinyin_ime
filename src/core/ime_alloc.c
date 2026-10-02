/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ime_alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#endif

#if defined(ESP_PLATFORM) && defined(CONFIG_SPIRAM)
#define IME_ALLOC_PSRAM 1
#else
#define IME_ALLOC_PSRAM 0
#endif

/* Small bookkeeping block placed in front of every allocation so that free()
 * can tell which heap the payload came from. Keeping the header in internal
 * RAM (the wrapper always uses heap_caps_malloc(MALLOC_CAP_8BIT) for it) is
 * what makes free() safe: we never have to read a PSRAM pointer we no longer
 * own. */
typedef struct {
    uint32_t magic;
    uint32_t heap; /* 0 = internal/default, 1 = PSRAM */
    size_t size;
} ime_alloc_hdr_t;

#define IME_ALLOC_MAGIC 0x494D4541u /* "IMEA" */

static size_t s_used;
static size_t s_blocks;

/* Deliberately leaked-free: statistics are best effort and never fatal. */
static void *raw_alloc(size_t size, int prefer_psram, int *from_psram)
{
#if IME_ALLOC_PSRAM
    if (prefer_psram) {
        void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p != NULL) {
            *from_psram = 1;
            return p;
        }
    }
    void *p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p != NULL) {
        *from_psram = 0;
        return p;
    }
    *from_psram = 0;
    return NULL;
#else
    (void)prefer_psram;
    *from_psram = 0;
    return malloc(size);
#endif
}

static void raw_free(void *ptr, int from_psram)
{
#if IME_ALLOC_PSRAM
    (void)from_psram;
    heap_caps_free(ptr);
#else
    (void)from_psram;
    free(ptr);
#endif
}

void *ime_malloc(size_t size)
{
    if (size == 0) {
        size = 1;
    }

    int from_psram = 0;
    ime_alloc_hdr_t *hdr = (ime_alloc_hdr_t *)raw_alloc(sizeof(*hdr) + size, 1, &from_psram);
    if (hdr == NULL) {
        return NULL;
    }

    hdr->magic = IME_ALLOC_MAGIC;
    hdr->heap = (uint32_t)from_psram;
    hdr->size = size;

    s_used += size;
    s_blocks++;

    return (void *)(hdr + 1);
}

void *ime_calloc(size_t n, size_t size)
{
    if (n == 0 || size == 0) {
        return ime_malloc(1);
    }

    size_t total = n * size;
    if (size != 0 && total / size != n) {
        return NULL; /* overflow */
    }

    void *p = ime_malloc(total);
    if (p != NULL) {
        memset(p, 0, total);
    }
    return p;
}

void *ime_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return ime_malloc(size);
    }
    if (size == 0) {
        ime_free(ptr);
        return NULL;
    }

    ime_alloc_hdr_t *hdr = ((ime_alloc_hdr_t *)ptr) - 1;
    if (hdr->magic != IME_ALLOC_MAGIC) {
        /* Not ours: fall back to the C library so a mixed caller cannot crash. */
#if IME_ALLOC_PSRAM
        return heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
#else
        return realloc(ptr, size);
#endif
    }

    if (hdr->size >= size) {
        s_used -= hdr->size - size;
        hdr->size = size;
        return ptr;
    }

    void *fresh = ime_malloc(size);
    if (fresh == NULL) {
        return NULL;
    }
    memcpy(fresh, ptr, hdr->size);
    ime_free(ptr);
    return fresh;
}

void ime_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    ime_alloc_hdr_t *hdr = ((ime_alloc_hdr_t *)ptr) - 1;
    if (hdr->magic != IME_ALLOC_MAGIC) {
        /* Allocated before our wrapper saw it (for example by the C library
         * during startup): hand it back to the default heap. */
        raw_free(ptr, 0);
        return;
    }

    s_used -= hdr->size;
    s_blocks--;

    hdr->magic = 0;
    raw_free(hdr, (int)hdr->heap);
}

size_t ime_alloc_used_bytes(void)
{
    return s_used;
}

size_t ime_alloc_live_blocks(void)
{
    return s_blocks;
}

#if defined(ESP_PLATFORM)
/* The linker wraps malloc/calloc/realloc/free so that libgooglepinyin (and
 * only it; nothing else in the image calls the wrapped names) lands in PSRAM. */
void *__wrap_malloc(size_t size)
{
    return ime_malloc(size);
}

void *__wrap_calloc(size_t n, size_t size)
{
    return ime_calloc(n, size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
    return ime_realloc(ptr, size);
}

void __wrap_free(void *ptr)
{
    ime_free(ptr);
}
#endif
