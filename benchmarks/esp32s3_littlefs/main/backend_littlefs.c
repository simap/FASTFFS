#include "vfs_bench.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_littlefs.h"

static bool mounted;

const char *bench_backend_name(void)
{
    return "LittleFS";
}

esp_err_t bench_backend_mount(bool format_if_mount_failed)
{
    if (mounted) {
        return ESP_OK;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = VFS_BENCH_BASE_PATH,
        .partition_label = VFS_BENCH_PARTITION_LABEL,
        .format_if_mount_failed = format_if_mount_failed,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err == ESP_OK) {
        mounted = true;
    }
    return err;
}

esp_err_t bench_backend_unmount(void)
{
    if (!mounted) {
        return ESP_OK;
    }
    esp_err_t err = esp_vfs_littlefs_unregister(VFS_BENCH_PARTITION_LABEL);
    if (err == ESP_OK) {
        mounted = false;
    }
    return err;
}

esp_err_t bench_backend_format(void)
{
    (void)bench_backend_unmount();
    return esp_littlefs_format(VFS_BENCH_PARTITION_LABEL);
}

esp_err_t bench_backend_info(size_t *total, size_t *used)
{
    return esp_littlefs_info(VFS_BENCH_PARTITION_LABEL, total, used);
}

void app_main(void)
{
    ESP_LOGI("littlefs_bench", "LittleFS ESP32-S3 benchmark starting");
    run_vfs_benchmarks();
    ESP_LOGI("littlefs_bench", "LittleFS ESP32-S3 benchmark done");
}
