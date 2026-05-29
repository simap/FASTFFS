#include "vfs_bench.h"

#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"
#include "wear_levelling.h"

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static bool s_mounted;

static esp_vfs_fat_mount_config_t mount_config(bool format_if_mount_failed)
{
    return (esp_vfs_fat_mount_config_t) {
        .format_if_mount_failed = format_if_mount_failed,
        .max_files = 8,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
}

const char *bench_backend_name(void)
{
    return "FATFS";
}

esp_err_t bench_backend_mount(bool format_if_mount_failed)
{
    if (s_mounted) {
        return ESP_OK;
    }

    esp_vfs_fat_mount_config_t conf = mount_config(format_if_mount_failed);
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        VFS_BENCH_BASE_PATH,
        VFS_BENCH_PARTITION_LABEL,
        &conf,
        &s_wl_handle);
    if (err == ESP_OK) {
        s_mounted = true;
    }
    return err;
}

esp_err_t bench_backend_unmount(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }

    esp_err_t err = esp_vfs_fat_spiflash_unmount_rw_wl(
        VFS_BENCH_BASE_PATH,
        s_wl_handle);
    if (err == ESP_OK) {
        s_mounted = false;
        s_wl_handle = WL_INVALID_HANDLE;
    }
    return err;
}

esp_err_t bench_backend_format(void)
{
    esp_vfs_fat_mount_config_t conf = mount_config(true);
    return esp_vfs_fat_spiflash_format_cfg_rw_wl(
        VFS_BENCH_BASE_PATH,
        VFS_BENCH_PARTITION_LABEL,
        &conf);
}

esp_err_t bench_backend_info(size_t *total, size_t *used)
{
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info(VFS_BENCH_BASE_PATH,
                                     &total_bytes,
                                     &free_bytes);
    if (err != ESP_OK) {
        return err;
    }
    if (total != NULL) {
        *total = (size_t)total_bytes;
    }
    if (used != NULL) {
        *used = (size_t)(total_bytes - free_bytes);
    }
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI("fatfs_bench", "FATFS ESP32-S3 benchmark starting");
    run_vfs_benchmarks();
    ESP_LOGI("fatfs_bench", "FATFS ESP32-S3 benchmark done");
}
