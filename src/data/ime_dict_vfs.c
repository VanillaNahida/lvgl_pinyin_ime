/*
 * SPDX-FileCopyrightText: 2026 lvgl_pinyin_ime contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ime_dict_vfs.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../core/ime_log.h"

/* The host unit test only needs the payload-size rule; the VFS itself is
 * ESP-IDF specific. The rule lives at the bottom of this file, outside the
 * guard, so test_apps/host_core_test can compile this translation unit with
 * -DIME_VFS_HOST_TEST and exercise it without esp_partition / esp_vfs. */
#ifndef IME_VFS_HOST_TEST

#include "esp_partition.h"
#include "esp_vfs.h"

static const char *TAG = "ime_vfs";

#define IME_VFS_MAX_FDS 4

/** One open file: which mount it belongs to plus its own offset. */
typedef struct {
    ime_vfs_mount_t *mount;
    off_t pos;
    bool in_use;
} ime_vfs_file_t;

/* ESP-IDF passes this back as the VFS context, so the open file table can be
 * global: a mount is identified by its path, exactly like a real file system. */
typedef struct {
    ime_vfs_mount_t mounts[IME_VFS_MAX_MOUNTS];
    ime_vfs_file_t files[IME_VFS_MAX_FDS];
    bool registered;
} ime_vfs_state_t;

static ime_vfs_state_t s_vfs;

/* ESP-IDF gives each mount its own esp_vfs_register() call, but the open file
 * table must stay shared, so the context passed to the callbacks is always
 * &s_vfs. */

static ime_vfs_mount_t *mount_by_path(const char *path)
{
    for (size_t i = 0; i < IME_VFS_MAX_MOUNTS; i++) {
        ime_vfs_mount_t *mount = &s_vfs.mounts[i];
        if (!mount->mounted) {
            continue;
        }
        /* The driver is asked for the path with the mount point already
         * stripped, so compare against local_path. Accept the full path too:
         * some ESP-IDF versions pass it through unchanged. */
        if (strcmp(mount->local_path, path) == 0 || strcmp(mount->path, path) == 0) {
            return mount;
        }
    }
    return NULL;
}

static ime_vfs_file_t *file_from_fd(int fd)
{
    if (fd < 0 || fd >= IME_VFS_MAX_FDS || !s_vfs.files[fd].in_use) {
        return NULL;
    }
    return &s_vfs.files[fd];
}

static int ime_vfs_open(void *ctx, const char *path, int flags, int mode)
{
    (void)ctx;
    (void)mode;

    ime_vfs_mount_t *mount = mount_by_path(path);
    if (mount == NULL) {
        IME_LOGW(TAG, "open(\"%s\") -> ENOENT", path);
        errno = ENOENT;
        return -1;
    }
    if ((flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) != 0) {
        errno = EROFS;
        return -1;
    }

    for (int fd = 0; fd < IME_VFS_MAX_FDS; fd++) {
        if (!s_vfs.files[fd].in_use) {
            s_vfs.files[fd].in_use = true;
            s_vfs.files[fd].pos = 0;
            s_vfs.files[fd].mount = mount;
            return fd;
        }
    }

    errno = EMFILE;
    return -1;
}

static int ime_vfs_close(void *ctx, int fd)
{
    (void)ctx;
    ime_vfs_file_t *f = file_from_fd(fd);
    if (f == NULL) {
        errno = EBADF;
        return -1;
    }
    f->in_use = false;
    f->mount = NULL;
    f->pos = 0;
    return 0;
}

static ssize_t ime_vfs_read(void *ctx, int fd, void *dst, size_t size)
{
    (void)ctx;
    ime_vfs_file_t *f = file_from_fd(fd);
    if (f == NULL || f->mount == NULL) {
        errno = EBADF;
        return -1;
    }
    if ((size_t)f->pos >= f->mount->size) {
        return 0;
    }

    size_t avail = f->mount->size - (size_t)f->pos;
    if (size > avail) {
        size = avail;
    }
    memcpy(dst, f->mount->data + f->pos, size);
    f->pos += (off_t)size;
    return (ssize_t)size;
}

static ssize_t ime_vfs_pread(void *ctx, int fd, void *dst, size_t size, off_t offset)
{
    (void)ctx;
    ime_vfs_file_t *f = file_from_fd(fd);
    if (f == NULL || f->mount == NULL) {
        errno = EBADF;
        return -1;
    }
    if (offset < 0 || (size_t)offset >= f->mount->size) {
        return 0;
    }

    size_t avail = f->mount->size - (size_t)offset;
    if (size > avail) {
        size = avail;
    }
    memcpy(dst, f->mount->data + offset, size);
    return (ssize_t)size;
}

static off_t ime_vfs_lseek(void *ctx, int fd, off_t offset, int whence)
{
    (void)ctx;
    ime_vfs_file_t *f = file_from_fd(fd);
    if (f == NULL || f->mount == NULL) {
        errno = EBADF;
        return -1;
    }

    off_t base;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = f->pos;
        break;
    case SEEK_END:
        base = (off_t)f->mount->size;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    off_t next = base + offset;
    if (next < 0) {
        errno = EINVAL;
        return -1;
    }
    f->pos = next;
    return next;
}

static void fill_stat(const ime_vfs_mount_t *mount, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_size = (off_t)mount->size;
    st->st_mode = S_IFREG | 0444;
}

static int ime_vfs_fstat(void *ctx, int fd, struct stat *st)
{
    (void)ctx;
    ime_vfs_file_t *f = file_from_fd(fd);
    if (f == NULL || f->mount == NULL) {
        errno = EBADF;
        return -1;
    }
    fill_stat(f->mount, st);
    return 0;
}

static int ime_vfs_stat(void *ctx, const char *path, struct stat *st)
{
    (void)ctx;
    ime_vfs_mount_t *mount = mount_by_path(path);
    if (mount == NULL) {
        errno = ENOENT;
        return -1;
    }
    fill_stat(mount, st);
    return 0;
}

static int ime_vfs_access(void *ctx, const char *path, int amode)
{
    (void)ctx;
    (void)amode;
    if (mount_by_path(path) == NULL) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

static bool register_vfs(void)
{
    if (s_vfs.registered) {
        return true;
    }

    static const esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_CONTEXT_PTR,
        .open_p = ime_vfs_open,
        .close_p = ime_vfs_close,
        .read_p = ime_vfs_read,
        .pread_p = ime_vfs_pread,
        .lseek_p = ime_vfs_lseek,
        .fstat_p = ime_vfs_fstat,
        .stat_p = ime_vfs_stat,
        .access_p = ime_vfs_access,
    };

    /* One registration per mount point: ESP-IDF keys the file system by the
     * base path, and all our mounts share the /ime prefix, so the first
     * registration covers them all. */
    esp_err_t err = esp_vfs_register("/ime", &vfs, &s_vfs);
    if (err != ESP_OK) {
        IME_LOGE(TAG, "esp_vfs_register failed: %s", esp_err_to_name(err));
        return false;
    }
    s_vfs.registered = true;
    return true;
}

#endif /* IME_VFS_HOST_TEST: the ESP-IDF VFS source ends here */

/* Unconditional: the firmware needs it to trim partition padding, and the host
 * unit test compiles this translation unit with -DIME_VFS_HOST_TEST to cover
 * it. */
size_t ime_vfs_payload_size(const void *data, size_t size)
{
    if (data == NULL || size == 0) {
        return 0;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    size_t payload = size;
    while (payload > 0 && bytes[payload - 1] == 0xFF) {
        payload--;
    }
    return payload;
}

#ifndef IME_VFS_HOST_TEST

ime_vfs_mount_t *ime_vfs_mount(const char *mount_point, const char *file_name, const void *data,
                               size_t size)
{
    if (mount_point == NULL || file_name == NULL || data == NULL || size == 0) {
        return NULL;
    }
    if (!register_vfs()) {
        return NULL;
    }

    /* Flash partitions are almost always larger than the image inside them: the
     * tail is erased flash, 0xFF, which must not be reported as file content. */
    size_t payload = ime_vfs_payload_size(data, size);
    if (payload == 0) {
        IME_LOGW(TAG, "%s/%s: image is empty (all 0xFF)", mount_point, file_name);
        return NULL;
    }
    const uint8_t *bytes = (const uint8_t *)data;

    for (size_t i = 0; i < IME_VFS_MAX_MOUNTS; i++) {
        ime_vfs_mount_t *mount = &s_vfs.mounts[i];
        if (mount->mounted) {
            continue;
        }

        if (strlen(mount_point) >= sizeof(mount->mount_point) ||
            strlen(mount_point) + 1 + strlen(file_name) >= sizeof(mount->path)) {
            IME_LOGE(TAG, "path too long: %s/%s", mount_point, file_name);
            return NULL;
        }

        strlcpy(mount->mount_point, mount_point, sizeof(mount->mount_point));
        snprintf(mount->path, sizeof(mount->path), "%s/%s", mount_point, file_name);
        snprintf(mount->local_path, sizeof(mount->local_path), "/%s", file_name);
        mount->data = bytes;
        mount->size = payload;
        mount->mounted = true;
        if (payload != size) {
            IME_LOGI(TAG, "mounted %s as \"%s\" (%u bytes of %u, %u bytes of flash padding skipped)",
                     mount->path, mount->local_path, (unsigned)payload, (unsigned)size,
                     (unsigned)(size - payload));
        } else {
            IME_LOGI(TAG, "mounted %s as \"%s\" (%u bytes) read-only", mount->path,
                     mount->local_path, (unsigned)payload);
        }
        return mount;
    }

    IME_LOGE(TAG, "no free mount slot for %s/%s", mount_point, file_name);
    return NULL;
}

void ime_vfs_unmount(ime_vfs_mount_t *mount)
{
    if (mount == NULL || !mount->mounted) {
        return;
    }

    /* Close any file still open on it. */
    for (int fd = 0; fd < IME_VFS_MAX_FDS; fd++) {
        if (s_vfs.files[fd].in_use && s_vfs.files[fd].mount == mount) {
            s_vfs.files[fd].in_use = false;
            s_vfs.files[fd].mount = NULL;
            s_vfs.files[fd].pos = 0;
        }
    }

    mount->mounted = false;
    mount->data = NULL;
    mount->size = 0;
    mount->path[0] = '\0';
    mount->local_path[0] = '\0';
    mount->mount_point[0] = '\0';
    if (mount->mapped) {
        esp_partition_munmap((esp_partition_mmap_handle_t)mount->mmap_handle);
        mount->mapped = false;
        mount->mmap_handle = 0;
    }
}

void ime_vfs_unmount_all(void)
{
    for (size_t i = 0; i < IME_VFS_MAX_MOUNTS; i++) {
        ime_vfs_unmount(&s_vfs.mounts[i]);
    }
    if (s_vfs.registered) {
        esp_vfs_unregister("/ime");
        s_vfs.registered = false;
    }
}

ime_vfs_mount_t *ime_vfs_mount_partition(const char *partition_label, uint8_t subtype,
                                         const char *mount_point, const char *file_name,
                                         size_t *out_size)
{
    if (partition_label == NULL) {
        return NULL;
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)subtype, partition_label);
    if (part == NULL) {
        IME_LOGW(TAG, "partition \"%s\" (subtype 0x%02X) not found", partition_label, subtype);
        return NULL;
    }

    const void *mapped = NULL;
    esp_partition_mmap_handle_t handle = 0;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &mapped, &handle);
    if (err != ESP_OK) {
        IME_LOGE(TAG, "esp_partition_mmap(%s) failed: %s", partition_label, esp_err_to_name(err));
        return NULL;
    }

    ime_vfs_mount_t *mount = ime_vfs_mount(mount_point, file_name, mapped, part->size);
    if (mount == NULL) {
        esp_partition_munmap(handle);
        return NULL;
    }

    mount->mmap_handle = (uint32_t)handle;
    mount->mapped = true;

    if (out_size != NULL) {
        *out_size = part->size;
    }
    return mount;
}

#endif /* IME_VFS_HOST_TEST */
