/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Read-only ESP-IDF VFS that presents a memory range as a file.
 *
 * Two consumers need a file, not a buffer:
 *
 *   - libgooglepinyin opens its system dictionary with fopen()
 *   - lv_binfont_create() opens the font with lv_fs, and the memory-buffer
 *     variant would require LV_USE_FS_MEMFS
 *
 * Both are served from a flash partition (or from a copy in PSRAM), so a
 * handful of read-only mounts is all that is needed. Writes always fail with
 * EROFS: the system dictionary and the fonts are read-only, the user dictionary
 * lives on a writable file system instead (ime_userdb).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Mounts the IME needs at the same time: dictionary and two fonts. */
#define IME_VFS_MAX_MOUNTS 3

/** One registered read-only file. */
typedef struct {
    char mount_point[24];
    /** Path as the caller sees it, e.g. "/ime/dict_pinyin.dat". */
    char path[64];
    /**
     * Path as the VFS callbacks see it: ESP-IDF strips the mount point before
     * calling the driver (translate_path() in vfs.c), so open() is asked for
     * "/dict_pinyin.dat", not for the full path. Matching against the wrong one
     * is an instant ENOENT.
     */
    char local_path[64];
    const uint8_t *data;
    size_t size;
    bool mounted;
    uint32_t mmap_handle; /**< esp_partition_mmap handle, 0 when not mapped */
    bool mapped;
} ime_vfs_mount_t;

/**
 * Register @p data as @p mount_point + "/" + @p file_name.
 *
 * @param out  receives the mount handle on success, may be NULL
 * @return the mount, or NULL when out of memory / slots
 */
ime_vfs_mount_t *ime_vfs_mount(const char *mount_point, const char *file_name, const void *data,
                               size_t size);

/** Unregister a mount. Safe with NULL and safe to call twice. */
void ime_vfs_unmount(ime_vfs_mount_t *mount);

/** Unregister everything, used by the tests and by deinit paths. */
void ime_vfs_unmount_all(void);

/**
 * Size of the real payload inside a flash image: everything up to the last byte
 * that is not erased flash (0xFF).
 *
 * Partitions are usually larger than the image inside them, and a file must not
 * report that padding: libgooglepinyin would read 0xFF past the end of its
 * dictionary and fail to parse it. Exposed for the host unit test.
 *
 * @return the payload size, or 0 when the whole image is erased
 */
size_t ime_vfs_payload_size(const void *data, size_t size);

/**
 * mmap() the flash partition @p partition_label (data subtype @p subtype) and
 * serve it as @p mount_point + "/" + @p file_name.
 *
 * @param out_size  receives the partition size in bytes, may be NULL
 * @return the mount, or NULL. The mapping stays alive until ime_vfs_unmount().
 */
ime_vfs_mount_t *ime_vfs_mount_partition(const char *partition_label, uint8_t subtype,
                                         const char *mount_point, const char *file_name,
                                         size_t *out_size);

#ifdef __cplusplus
}
#endif
