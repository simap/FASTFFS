#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define VFS_BENCH_BASE_PATH "/fs"
#define VFS_BENCH_PARTITION_LABEL "storage"

const char *bench_backend_name(void);
esp_err_t bench_backend_mount(bool format_if_mount_failed);
esp_err_t bench_backend_unmount(void);
esp_err_t bench_backend_format(void);
esp_err_t bench_backend_info(size_t *total, size_t *used);

void run_vfs_benchmarks(void);
