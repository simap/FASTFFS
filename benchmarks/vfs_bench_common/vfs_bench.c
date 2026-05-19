/*
 * SPDX-License-Identifier: MIT
 *
 * benchfs adapter for ESP-IDF VFS-backed filesystems.
 */

#include "vfs_bench.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "benchfs.h"
#include "benchfs_esp.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"

static const char *TAG = "vfs_bench";

typedef struct {
    const esp_partition_t *part;
} vfs_adapter_t;

static int64_t adapter_now_us(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time();
}

static void adapter_vlog(void *ctx, benchfs_log_level_t level, const char *fmt,
                         va_list ap)
{
    (void)ctx;
    char line[768];
    vsnprintf(line, sizeof(line), fmt, ap);
    if (level == BENCHFS_LOG_ERROR) {
        ESP_LOGE(TAG, "%s", line);
    } else {
        ESP_LOGI(TAG, "%s", line);
    }
}

static const char *adapter_error_name(void *ctx, int rc)
{
    (void)ctx;
    if (rc == BENCHFS_OK) {
        return "ESP_OK";
    }
    if (rc == BENCHFS_ERR_NO_SPACE) {
        return "NO_SPACE";
    }
    if (rc > 0) {
        return esp_err_to_name(rc);
    }
    return strerror(-rc);
}

static void make_path(char *dst, size_t len, const char *name)
{
    snprintf(dst, len, "%s/%s", VFS_BENCH_BASE_PATH, name);
}

static int errno_to_benchfs(int err)
{
    return err == ENOSPC ? BENCHFS_ERR_NO_SPACE : -err;
}

static int adapter_setup(void *ctx)
{
    vfs_adapter_t *a = ctx;
    if (a->part != NULL) {
        return BENCHFS_OK;
    }
    a->part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                       ESP_PARTITION_SUBTYPE_ANY,
                                       VFS_BENCH_PARTITION_LABEL);
    if (a->part == NULL) {
        ESP_LOGE(TAG, "partition '%s' not found", VFS_BENCH_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "using partition '%s' at 0x%lx size 0x%lx",
             a->part->label, (unsigned long)a->part->address,
             (unsigned long)a->part->size);
    return BENCHFS_OK;
}

static int adapter_erase_storage(void *ctx)
{
    vfs_adapter_t *a = ctx;
    (void)bench_backend_unmount();
    return esp_partition_erase_range(a->part, 0, a->part->size);
}

static int adapter_format(void *ctx)
{
    (void)ctx;
    return bench_backend_format();
}

static int adapter_mount(void *ctx)
{
    (void)ctx;
    return bench_backend_mount(false);
}

static int adapter_unmount(void *ctx)
{
    (void)ctx;
    return bench_backend_unmount();
}

static int adapter_open(void *ctx, const char *name, uint32_t flags, void **file)
{
    (void)ctx;
    char path[64];
    make_path(path, sizeof(path), name);
    const char *mode = NULL;
    if (flags & BENCHFS_OPEN_WRITE_TRUNC) {
        mode = "wb";
    } else if (flags & BENCHFS_OPEN_READ) {
        mode = "rb";
    }
    if (mode == NULL) {
        return -EINVAL;
    }
    FILE *f = fopen(path, mode);
    if (f == NULL) {
        return errno_to_benchfs(errno);
    }
    *file = f;
    return BENCHFS_OK;
}

static int adapter_write(void *ctx, void *file, const void *buf, size_t len)
{
    (void)ctx;
    size_t written = fwrite(buf, 1, len, (FILE *)file);
    if (written != len) {
        return errno_to_benchfs(errno ? errno : EIO);
    }
    if (fflush((FILE *)file) != 0) {
        return errno_to_benchfs(errno ? errno : EIO);
    }
    return BENCHFS_OK;
}

static int adapter_read(void *ctx, void *file, void *buf, size_t len,
                        size_t *read_len)
{
    (void)ctx;
    size_t got = fread(buf, 1, len, (FILE *)file);
    if (got == 0 && ferror((FILE *)file)) {
        return errno_to_benchfs(errno ? errno : EIO);
    }
    *read_len = got;
    return BENCHFS_OK;
}

static int adapter_fstat(void *ctx, void *file, uint32_t *size)
{
    (void)ctx;
    struct stat st;
    if (fstat(fileno((FILE *)file), &st) != 0) {
        return errno_to_benchfs(errno ? errno : EIO);
    }
    *size = (uint32_t)st.st_size;
    return BENCHFS_OK;
}

static int adapter_close(void *ctx, void *file)
{
    (void)ctx;
    return fclose((FILE *)file) == 0 ? BENCHFS_OK :
        errno_to_benchfs(errno ? errno : EIO);
}

static int adapter_delete_file(void *ctx, const char *name)
{
    (void)ctx;
    char path[64];
    make_path(path, sizeof(path), name);
    return unlink(path) == 0 ? BENCHFS_OK :
        errno_to_benchfs(errno ? errno : EIO);
}

static int adapter_exists(void *ctx, const char *name, bool *exists)
{
    (void)ctx;
    char path[64];
    struct stat st;
    make_path(path, sizeof(path), name);
    if (stat(path, &st) == 0) {
        *exists = true;
        return BENCHFS_OK;
    }
    if (errno == ENOENT) {
        *exists = false;
        return BENCHFS_OK;
    }
    return errno_to_benchfs(errno ? errno : EIO);
}

static int adapter_list_count(void *ctx, size_t *count)
{
    (void)ctx;
    DIR *dir = opendir(VFS_BENCH_BASE_PATH);
    if (dir == NULL) {
        return errno_to_benchfs(errno ? errno : EIO);
    }
    size_t n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            n++;
        }
    }
    int rc = closedir(dir) == 0 ? BENCHFS_OK :
        errno_to_benchfs(errno ? errno : EIO);
    *count = n;
    return rc;
}

static int adapter_fsinfo(void *ctx, benchfs_info_t *info)
{
    (void)ctx;
    size_t total = 0;
    size_t used = 0;
    esp_err_t rc = bench_backend_info(&total, &used);
    if (rc != ESP_OK) {
        return rc;
    }
    info->total_valid = true;
    info->used_valid = true;
    info->total_bytes = (uint32_t)total;
    info->used_bytes = (uint32_t)used;
    return BENCHFS_OK;
}

static void adapter_log_config(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "config backend=%s base_path=%s partition=%s",
             bench_backend_name(), VFS_BENCH_BASE_PATH,
             VFS_BENCH_PARTITION_LABEL);
}

static void adapter_log_backend_info(void *ctx, const char *label)
{
    vfs_adapter_t *a = ctx;
    ESP_LOGI(TAG, "%s partition_bytes=%lu", label,
             (unsigned long)(a->part ? a->part->size : 0));
}

static bool measure_fopen_delta(const char *path, const char *mode,
                                uint32_t *delta_out)
{
    uint32_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    FILE *f = fopen(path, mode);
    uint32_t after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (f == NULL) {
        return false;
    }
    if (before > after && before - after > *delta_out) {
        *delta_out = before - after;
    }
    fclose(f);
    return true;
}

static int adapter_memory_info(void *ctx, benchfs_memory_info_t *info)
{
    (void)ctx;
    memset(info, 0, sizeof(*info));
    (void)bench_backend_unmount();

    uint32_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    int rc = bench_backend_mount(false);
    uint32_t after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (rc == BENCHFS_OK) {
        info->base_valid = true;
        info->base_bytes = before > after ? before - after : 0;
    }

    char path[64];
    make_path(path, sizeof(path), "__bench_mem_probe.bin");
    uint32_t open_delta = 0;
    bool open_valid = false;
    open_valid |= measure_fopen_delta(path, "wb", &open_delta);
    FILE *seed = fopen(path, "wb");
    if (seed != NULL) {
        static const uint8_t byte = 0xa5;
        (void)fwrite(&byte, 1, 1, seed);
        (void)fclose(seed);
    }
    open_valid |= measure_fopen_delta(path, "rb", &open_delta);
    (void)unlink(path);
    if (open_valid) {
        info->open_file_valid = true;
        info->open_file_bytes = open_delta;
    }
    return BENCHFS_OK;
}

void run_vfs_benchmarks(void)
{
    static vfs_adapter_t adapter;
    benchfs_config_t cfg;
    char name[64];
    snprintf(name, sizeof(name), "%s ESP32-S3", bench_backend_name());
    benchfs_default_config(&cfg, name);
    cfg.erase_before_baseline_format = true;
    cfg.erase_before_churn_format = true;

    const benchfs_ops_t ops = {
        .now_us = adapter_now_us,
        .vlog = adapter_vlog,
        .error_name = adapter_error_name,
        .setup = adapter_setup,
        .erase_storage = adapter_erase_storage,
        .format = adapter_format,
        .mount = adapter_mount,
        .unmount = adapter_unmount,
        .open = adapter_open,
        .write = adapter_write,
        .read = adapter_read,
        .fstat = adapter_fstat,
        .close = adapter_close,
        .delete_file = adapter_delete_file,
        .exists = adapter_exists,
        .list_count = adapter_list_count,
        .fsinfo = adapter_fsinfo,
        .memory_info = adapter_memory_info,
        .stack_used_bytes = benchfs_esp_current_stack_used_bytes,
        .run_noop_stack_baseline = benchfs_esp_run_noop_stack_baseline,
        .log_config = adapter_log_config,
        .log_backend_info = adapter_log_backend_info,
    };

    (void)benchfs_run(&cfg, &ops, &adapter);
}
