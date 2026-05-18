#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "benchfs.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "jesfs.h"

#define JESFS_PARTITION_LABEL "jesfs"
#define JESFS_MAX_OPEN_FILES 2

static const char *TAG = "jesfs_bench";

typedef struct {
    const esp_partition_t *part;
    bool file_used[JESFS_MAX_OPEN_FILES];
    FS_DESC files[JESFS_MAX_OPEN_FILES];
} jesfs_adapter_t;

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
        return "0";
    }
    if (rc == BENCHFS_ERR_NO_SPACE) {
        return "NO_SPACE";
    }
    return esp_err_to_name(rc);
}

static int adapter_setup(void *ctx)
{
    jesfs_adapter_t *a = ctx;
    if (a->part != NULL) {
        return BENCHFS_OK;
    }
    a->part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                       ESP_PARTITION_SUBTYPE_ANY,
                                       JESFS_PARTITION_LABEL);
    if (a->part == NULL) {
        ESP_LOGE(TAG, "partition '%s' not found", JESFS_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "using partition '%s' at 0x%lx size 0x%lx",
             a->part->label, (unsigned long)a->part->address,
             (unsigned long)a->part->size);
    return BENCHFS_OK;
}

static int adapter_erase_storage(void *ctx)
{
    jesfs_adapter_t *a = ctx;
    (void)fs_deepsleep();
    esp_err_t err = esp_partition_erase_range(a->part, 0, a->part->size);
    if (err != ESP_OK) {
        return err;
    }
    (void)fs_start(FS_START_NORMAL);
    return BENCHFS_OK;
}

static int adapter_format(void *ctx)
{
    (void)ctx;
    return fs_format(FS_FORMAT_SOFT);
}

static int adapter_mount(void *ctx)
{
    (void)ctx;
    return fs_start(FS_START_NORMAL);
}

static int adapter_unmount(void *ctx)
{
    (void)ctx;
    return fs_deepsleep();
}

static int adapter_open(void *ctx, const char *name, uint32_t flags, void **file)
{
    jesfs_adapter_t *a = ctx;
    FS_DESC *f = NULL;
    for (size_t i = 0; i < JESFS_MAX_OPEN_FILES; ++i) {
        if (!a->file_used[i]) {
            a->file_used[i] = true;
            f = &a->files[i];
            memset(f, 0, sizeof(*f));
            break;
        }
    }
    if (f == NULL) {
        return -1;
    }

    uint8_t jesfs_flags = 0;
    if (flags & BENCHFS_OPEN_WRITE_TRUNC) {
        jesfs_flags = SF_OPEN_CREATE | SF_OPEN_WRITE;
    } else if (flags & BENCHFS_OPEN_READ) {
        jesfs_flags = SF_OPEN_READ;
    } else {
        return -1;
    }

    int rc = fs_open(f, (char *)name, jesfs_flags);
    if (rc != 0) {
        for (size_t i = 0; i < JESFS_MAX_OPEN_FILES; ++i) {
            if (f == &a->files[i]) {
                a->file_used[i] = false;
                break;
            }
        }
        return rc;
    }
    *file = f;
    return BENCHFS_OK;
}

static int adapter_write(void *ctx, void *file, const void *buf, size_t len)
{
    (void)ctx;
    int rc = fs_write((FS_DESC *)file, (uint8_t *)buf, (uint32_t)len);
    return rc == 0 ? BENCHFS_OK : rc;
}

static int adapter_read(void *ctx, void *file, void *buf, size_t len,
                        size_t *read_len)
{
    (void)ctx;
    int32_t rc = fs_read((FS_DESC *)file, buf, (uint32_t)len);
    if (rc < 0) {
        return (int)rc;
    }
    *read_len = (size_t)rc;
    return BENCHFS_OK;
}

static int adapter_fstat(void *ctx, void *file, uint32_t *size)
{
    (void)ctx;
    *size = ((FS_DESC *)file)->file_len;
    return BENCHFS_OK;
}

static int adapter_close(void *ctx, void *file)
{
    jesfs_adapter_t *a = ctx;
    int rc = fs_close((FS_DESC *)file);
    for (size_t i = 0; i < JESFS_MAX_OPEN_FILES; ++i) {
        if (file == &a->files[i]) {
            a->file_used[i] = false;
            break;
        }
    }
    return rc == 0 ? BENCHFS_OK : rc;
}

static int adapter_delete_file(void *ctx, const char *name)
{
    FS_DESC f;
    (void)ctx;
    int rc = fs_open(&f, (char *)name, SF_OPEN_READ);
    if (rc != 0) {
        return rc;
    }
    rc = fs_delete(&f);
    return rc == 0 ? BENCHFS_OK : rc;
}

static int adapter_exists(void *ctx, const char *name, bool *exists)
{
    (void)ctx;
    int rc = fs_notexists((char *)name);
    *exists = rc == 0;
    return BENCHFS_OK;
}

static int adapter_list_count(void *ctx, size_t *count)
{
    (void)ctx;
    FS_STAT st;
    size_t active = 0;
    for (uint16_t i = 0; i < 1200; ++i) {
        int rc = fs_info(&st, i);
        if (rc == FS_STAT_INDEX) {
            break;
        }
        if (rc & FS_STAT_ACTIVE) {
            active++;
        } else if (rc < 0) {
            return rc;
        }
    }
    *count = active;
    return BENCHFS_OK;
}

static int adapter_fsinfo(void *ctx, benchfs_info_t *info)
{
    (void)ctx;
    uint32_t used = sflash_info.total_flash_size >
                            sflash_info.available_disk_size ?
                        sflash_info.total_flash_size -
                            sflash_info.available_disk_size :
                        0;
    info->total_valid = true;
    info->used_valid = true;
    info->file_count_valid = true;
    info->total_bytes = sflash_info.total_flash_size;
    info->used_bytes = used;
    size_t file_count = 0;
    (void)adapter_list_count(ctx, &file_count);
    info->file_count = (uint32_t)file_count;
    return BENCHFS_OK;
}

static void adapter_log_config(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "config backend=JesFS partition=%s max_open_files=%d",
             JESFS_PARTITION_LABEL, JESFS_MAX_OPEN_FILES);
}

static void adapter_log_backend_info(void *ctx, const char *label)
{
    jesfs_adapter_t *a = ctx;
    ESP_LOGI(TAG, "%s partition_bytes=%lu available=%lu total=%lu",
             label, (unsigned long)(a->part ? a->part->size : 0),
             (unsigned long)sflash_info.available_disk_size,
             (unsigned long)sflash_info.total_flash_size);
}

void app_main(void)
{
    static jesfs_adapter_t adapter;
    benchfs_config_t cfg;
    benchfs_default_config(&cfg, "JesFS ESP32-S3");
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
        .log_config = adapter_log_config,
        .log_backend_info = adapter_log_backend_info,
    };

    (void)benchfs_run(&cfg, &ops, &adapter);
}
