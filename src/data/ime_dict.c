/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ime_dict.h"
#include "ime_dict_vfs.h"
#include "../core/ime_alloc.h"
#include "../core/ime_log.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_LV_PINYIN_IME_DICT_SRC_SDCARD)
#include "esp_vfs_fat.h"
#endif

static const char *TAG = "ime_dict";

/* Binary blob linked into the application by CMake (target_add_binary_data).
 * The host test links no blob, so it gets an empty stand-in; the embedded path
 * then reports "dictionary is empty" instead of failing to link. */
#if defined(ESP_PLATFORM)
extern const uint8_t dict_pinyin_dat_start[] asm("_binary_dict_pinyin_dat_start");
extern const uint8_t dict_pinyin_dat_end[] asm("_binary_dict_pinyin_dat_end");
#else
static const uint8_t dict_pinyin_dat_start[1];
static const uint8_t dict_pinyin_dat_end[1];
#endif

static char s_path[64];
static bool s_prepared;
static size_t s_size;
static void *s_owned_blob;
static ime_vfs_mount_t *s_mount;

#if defined(CONFIG_LV_PINYIN_IME_DICT_SRC_SDCARD)
#define IME_DICT_MOUNT_POINT "/sdcard"
#else
/* Shared with the VFS registration in ime_dict_vfs.c. */
#define IME_DICT_MOUNT_POINT "/ime"
#endif

/* The VFS implementation (ime_dict_vfs.c) is ESP-IDF only. Host builds link
 * this fallback instead, so the code paths stay identical to the firmware while
 * the host test can still compile the file. */
#if !defined(ESP_PLATFORM)
__attribute__((weak)) ime_vfs_mount_t *ime_vfs_mount_partition(const char *partition_label,
                                                              uint8_t subtype,
                                                              const char *mount_point,
                                                              const char *file_name,
                                                              size_t *out_size)
{
    (void)partition_label;
    (void)subtype;
    (void)mount_point;
    (void)file_name;
    (void)out_size;
    return NULL;
}

__attribute__((weak)) ime_vfs_mount_t *ime_vfs_mount(const char *mount_point, const char *file_name,
                                                     const void *data, size_t size)
{
    (void)mount_point;
    (void)file_name;
    (void)data;
    (void)size;
    return NULL;
}

__attribute__((weak)) void ime_vfs_unmount(ime_vfs_mount_t *mount)
{
    (void)mount;
}
#endif

static bool prepare_partition(void)
{
#if defined(CONFIG_LV_PINYIN_IME_DICT_PARTITION)
    const char *label = CONFIG_LV_PINYIN_IME_DICT_PARTITION;
#else
    const char *label = "dict";
#endif
    s_mount = ime_vfs_mount_partition(label, 0x40, IME_DICT_MOUNT_POINT, "dict_pinyin.dat", NULL);
    if (s_mount == NULL) {
        return false;
    }
    /* The file size is the trimmed payload, which is what the engine will read;
     * the partition size is usually larger and is not interesting here. */
    s_size = s_mount->size;
    snprintf(s_path, sizeof(s_path), "%s", s_mount->path);
    return true;
}

#if !defined(ESP_PLATFORM)
/**
 * Host build (unit tests and the LVGL render harness): read the dictionary file
 * directly. There is no partition and no VFS here, and the embedded blob is a
 * placeholder, so walk the file system instead. Set IME_DICT to override the
 * path.
 */
static bool prepare_host_file(void)
{
    const char *path = getenv("IME_DICT");
    if (path == NULL || path[0] == '\0') {
        path = "data/dict/dict_pinyin.dat";
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        IME_LOGE(TAG, "cannot open %s (run tools/gen_dict.py)", path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fclose(fp);
    if (size <= 0) {
        IME_LOGE(TAG, "%s is empty", path);
        return false;
    }

    snprintf(s_path, sizeof(s_path), "%s", path);
    s_size = (size_t)size;
    return true;
}
#endif /* !ESP_PLATFORM */

static bool prepare_embedded(void)
{
    size_t blob_size = (size_t)(dict_pinyin_dat_end - dict_pinyin_dat_start);
    if (blob_size == 0) {
        IME_LOGE(TAG, "embedded dictionary is empty (was data/dict/dict_pinyin.dat present at build time?)");
        return false;
    }

    /* Copy out of DROM/IRAM into PSRAM so the partition-less build behaves like
     * the others and the flash cache is not thrashed by the loader. */
    s_owned_blob = ime_malloc(blob_size);
    const void *image = dict_pinyin_dat_start;
    if (s_owned_blob != NULL) {
        memcpy(s_owned_blob, dict_pinyin_dat_start, blob_size);
        image = s_owned_blob;
    } else {
        IME_LOGW(TAG, "no room to copy the embedded dictionary, serving it from flash");
    }

    s_mount = ime_vfs_mount(IME_DICT_MOUNT_POINT, "dict_pinyin.dat", image, blob_size);
    if (s_mount == NULL) {
        if (s_owned_blob != NULL) {
            ime_free(s_owned_blob);
            s_owned_blob = NULL;
        }
        return false;
    }

    s_size = blob_size;
    snprintf(s_path, sizeof(s_path), "%s", s_mount->path);
    return true;
}

#if defined(CONFIG_LV_PINYIN_IME_DICT_SRC_SDCARD)
static bool prepare_sdcard(void)
{
    const char *configured = CONFIG_LV_PINYIN_IME_DICT_SD_PATH;
    const char *path = (configured != NULL && configured[0] != '\0') ? configured
                                                                    : "/sdcard/dict_pinyin.dat";
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        IME_LOGE(TAG, "cannot open %s", path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fclose(fp);
    if (size <= 0) {
        IME_LOGE(TAG, "%s is empty", path);
        return false;
    }
    snprintf(s_path, sizeof(s_path), "%s", path);
    s_size = (size_t)size;
    return true;
}
#endif /* CONFIG_LV_PINYIN_IME_DICT_SRC_SDCARD */

bool ime_dict_prepare(char *out_path, size_t out_path_size)
{
    if (out_path == NULL || out_path_size == 0) {
        return false;
    }

    if (s_prepared) {
        snprintf(out_path, out_path_size, "%s", s_path);
        return true;
    }

    bool ok;
#if !defined(ESP_PLATFORM)
    ok = prepare_host_file();
#elif defined(CONFIG_LV_PINYIN_IME_DICT_SRC_SDCARD)
    ok = prepare_sdcard();
#elif defined(CONFIG_LV_PINYIN_IME_DICT_SRC_EMBEDDED)
    ok = prepare_embedded();
#else
    ok = prepare_partition();
    if (!ok) {
        IME_LOGW(TAG, "dictionary partition unavailable, trying the embedded copy");
        ok = prepare_embedded();
    }
#endif

    if (!ok) {
        return false;
    }

    s_prepared = true;
    snprintf(out_path, out_path_size, "%s", s_path);
    IME_LOGI(TAG, "system dictionary: %s (%u bytes)", s_path, (unsigned)s_size);
    return true;
}

void ime_dict_release(void)
{
    ime_vfs_unmount(s_mount);
    s_mount = NULL;
    if (s_owned_blob != NULL) {
        ime_free(s_owned_blob);
        s_owned_blob = NULL;
    }
    s_prepared = false;
    s_size = 0;
    s_path[0] = '\0';
}

size_t ime_dict_size(void)
{
    return s_size;
}

bool ime_dict_self_test(void)
{
    if (!s_prepared) {
        IME_LOGW(TAG, "self test before prepare");
        return false;
    }

    errno = 0;
    FILE *fp = fopen(s_path, "rb");
    if (fp == NULL) {
        IME_LOGE(TAG, "self test: fopen(%s) failed, errno=%d", s_path, errno);
        return false;
    }

    /* The engine reads the whole file, so prove the whole file is readable:
     * a short or failed read means the mapping or the reported size is wrong. */
    uint8_t buf[4096];
    size_t total = 0;
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), fp)) > 0) {
        total += got;
    }
    bool read_error = ferror(fp) != 0;
    fclose(fp);

    IME_LOGI(TAG, "self test: read %u of %u bytes from %s%s", (unsigned)total, (unsigned)s_size,
             s_path, read_error ? " (read error)" : "");
    return total == s_size && !read_error;
}

const char *ime_dict_user_path(void)
{
#if defined(CONFIG_LV_PINYIN_IME_USER_PARTITION)
    /* The user dictionary lives on the FAT partition mounted by the
     * application (ime_userdb owns the mount); the engine only needs the
     * path. */
    return "/usr/ime_user.dat";
#else
    return NULL;
#endif
}
