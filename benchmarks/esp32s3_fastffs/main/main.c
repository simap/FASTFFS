#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_log_write.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "benchfs.h"
#include "benchfs_esp.h"
#include "fastffs/fastffs.h"

#define FASTFFS_PARTITION_LABEL "storage"

#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_NONE
#define FASTFFS_INDEX_HEADS 0
#elif FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
#define FASTFFS_INDEX_HEADS FFFS_SLOT_COUNT
#else
#define FASTFFS_INDEX_HEADS FFFS_INDEX_HASH_TABLE_SIZE
#endif

#ifndef FASTFFS_SCRATCH_SIZE
#define FASTFFS_SCRATCH_SIZE 512
#endif

#define FASTFFS_ALLOC_MAP_WORDS (((4 * 1024 * 1024) / 4096 + 31) / 32)
#define FASTFFS_SECTOR_DATA_BYTES (4096 - 64 - 12)
#define FASTFFS_CHURN_GC_POLICY_FIXED 1
#define FASTFFS_CHURN_GC_POLICY_NONE 2
#define FASTFFS_CHURN_GC_POLICY_DEBT 3
#define FASTFFS_BENCHFS_MAX_OPEN_FILES 2
#ifndef FASTFFS_CHURN_GC_POLICY
#define FASTFFS_CHURN_GC_POLICY FASTFFS_CHURN_GC_POLICY_NONE
#endif
#ifndef FASTFFS_CHURN_FIXED_GC_STEPS
#define FASTFFS_CHURN_FIXED_GC_STEPS 16
#endif
#ifndef FASTFFS_CHURN_DEBT_MAX_STEPS
#define FASTFFS_CHURN_DEBT_MAX_STEPS 16
#endif
#ifndef FASTFFS_CHURN_DEBT_SCAN_MULTIPLIER
#define FASTFFS_CHURN_DEBT_SCAN_MULTIPLIER 2
#endif

static const char *TAG = "fastffs_benchfs";

typedef enum {
    RAW_STACK_BASELINE = 0,
    RAW_STACK_READ_4,
    RAW_STACK_READ_256,
    RAW_STACK_READ_4096,
    RAW_STACK_WRITE_256,
    RAW_STACK_ERASE_4096,
    RAW_STACK_ERASE_65536,
} raw_stack_op_t;

typedef struct {
    const esp_partition_t *part;
    TaskHandle_t waiter;
    raw_stack_op_t op;
    uint32_t used_bytes;
    esp_err_t err;
} raw_stack_task_t;

static uint8_t s_raw_read_buf[4096];
static uint8_t s_raw_write_buf[4096];

typedef struct {
    const esp_partition_t *part;
    struct fffs_backend backend;
    struct fffs fs;
    uint32_t index_cache[
        ((FFFS_INDEX_CACHE_BYTES(FASTFFS_INDEX_HEADS) + sizeof(uint32_t) - 1u) /
         sizeof(uint32_t)) ?
        ((FFFS_INDEX_CACHE_BYTES(FASTFFS_INDEX_HEADS) + sizeof(uint32_t) - 1u) /
         sizeof(uint32_t)) : 1u];
    uint8_t scratch[FASTFFS_SCRATCH_SIZE];
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    uint32_t alloc_map[FASTFFS_ALLOC_MAP_WORDS];
#endif
    bool mounted;
    uint32_t gc_reclaim_debt;
    uint32_t gc_scan_debt;
    bool file_used[FASTFFS_BENCHFS_MAX_OPEN_FILES];
    struct fffs_file files[FASTFFS_BENCHFS_MAX_OPEN_FILES];
} fastffs_adapter_t;

static int partition_read(void *ctx, size_t offset, void *buffer, size_t size)
{
    return esp_partition_read((const esp_partition_t *)ctx, offset, buffer,
                              size) == ESP_OK ? 0 : -1;
}

static int partition_program(void *ctx, size_t offset, const void *buffer,
                             size_t size)
{
    return esp_partition_write((const esp_partition_t *)ctx, offset, buffer,
                               size) == ESP_OK ? 0 : -1;
}

static int partition_erase(void *ctx, size_t offset, size_t size)
{
    return esp_partition_erase_range((const esp_partition_t *)ctx, offset,
                                     size) == ESP_OK ? 0 : -1;
}

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
    if (rc == BENCHFS_ERR_NO_SPACE) {
        return "NO_SPACE";
    }
    if (rc <= 0) {
        return fffs_status_name(rc);
    }
    return esp_err_to_name(rc);
}

static uint32_t sectors_for_payload(uint32_t size)
{
    return (size + FASTFFS_SECTOR_DATA_BYTES - 1) / FASTFFS_SECTOR_DATA_BYTES;
}

static const char *gc_policy_name(void)
{
#if FASTFFS_CHURN_GC_POLICY == FASTFFS_CHURN_GC_POLICY_FIXED
    return "fixed";
#elif FASTFFS_CHURN_GC_POLICY == FASTFFS_CHURN_GC_POLICY_NONE
    return "none";
#elif FASTFFS_CHURN_GC_POLICY == FASTFFS_CHURN_GC_POLICY_DEBT
    return "debt";
#else
    return "unknown";
#endif
}

static int adapter_setup(void *ctx)
{
    fastffs_adapter_t *a = ctx;
    if (a->part != NULL) {
        return BENCHFS_OK;
    }
    a->part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                       ESP_PARTITION_SUBTYPE_ANY,
                                       FASTFFS_PARTITION_LABEL);
    if (a->part == NULL) {
        ESP_LOGE(TAG, "partition '%s' not found", FASTFFS_PARTITION_LABEL);
        return -1;
    }
    a->backend = (struct fffs_backend){
        .ctx = (void *)a->part,
        .size = a->part->size,
        .read_granule = 1,
        .program_granule = 1,
        .read = partition_read,
        .program = partition_program,
        .erase = partition_erase,
    };
    ESP_LOGI(TAG, "using partition '%s' at 0x%lx size 0x%lx",
             a->part->label, (unsigned long)a->part->address,
             (unsigned long)a->part->size);
    return BENCHFS_OK;
}

static int adapter_unmount(void *ctx)
{
    fastffs_adapter_t *a = ctx;
    if (a->mounted) {
        fffs_unmount(&a->fs);
        a->mounted = false;
    }
    return BENCHFS_OK;
}

static int adapter_erase_storage(void *ctx)
{
    fastffs_adapter_t *a = ctx;
    adapter_unmount(ctx);
    return esp_partition_erase_range(a->part, 0, a->part->size) == ESP_OK ?
        BENCHFS_OK : FFFS_ERR_IO;
}

static int adapter_format(void *ctx)
{
    fastffs_adapter_t *a = ctx;
    struct fffs_format_options opts = {
        .index_sectors = FFFS_DEFAULT_INDEX_SECTORS,
        .sector_size = FFFS_SECTOR_4K,
    };
    return fffs_format(&a->backend, &opts);
}

static int adapter_mount(void *ctx)
{
    fastffs_adapter_t *a = ctx;
    struct fffs_mount_options opts = {
        .index_cache = a->index_cache,
        .index_cache_size = sizeof(a->index_cache),
        .index_hash_table_size = FASTFFS_INDEX_HEADS,
        .scratch = a->scratch,
        .scratch_size = sizeof(a->scratch),
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
        .alloc_map = a->alloc_map,
        .alloc_map_words = FASTFFS_ALLOC_MAP_WORDS,
#endif
    };
    int rc = fffs_mount(&a->fs, &a->backend, &opts);
    a->mounted = rc == FFFS_OK;
    a->gc_reclaim_debt = 0;
    a->gc_scan_debt = 0;
    return rc;
}

static int adapter_open(void *ctx, const char *name, uint32_t flags, void **file)
{
    fastffs_adapter_t *a = ctx;
    struct fffs_file *f = NULL;
    for (size_t i = 0; i < FASTFFS_BENCHFS_MAX_OPEN_FILES; ++i) {
        if (!a->file_used[i]) {
            a->file_used[i] = true;
            f = &a->files[i];
            memset(f, 0, sizeof(*f));
            break;
        }
    }
    if (!f) {
        return FFFS_ERR_NOMEM;
    }
    uint32_t fffs_flags = 0;
    if (flags & BENCHFS_OPEN_READ) {
        fffs_flags |= FFFS_O_RDONLY;
    }
    if (flags & BENCHFS_OPEN_WRITE_TRUNC) {
        fffs_flags |= FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC;
    }
    int rc = fffs_open(&a->fs, f, name, fffs_flags);
    if (rc != FFFS_OK) {
        for (size_t i = 0; i < FASTFFS_BENCHFS_MAX_OPEN_FILES; ++i) {
            if (f == &a->files[i]) {
                a->file_used[i] = false;
                break;
            }
        }
        return rc;
    }
    *file = f;
    return FFFS_OK;
}

static int adapter_write(void *ctx, void *file, const void *buf, size_t len)
{
    (void)ctx;
    size_t written = 0;
    int rc = fffs_write((struct fffs_file *)file, buf, len, &written);
    if (rc == FFFS_ERR_NO_SPACE) {
        return BENCHFS_ERR_NO_SPACE;
    }
    return rc == FFFS_OK && written == len ? FFFS_OK :
        (rc == FFFS_OK ? FFFS_ERR_IO : rc);
}

static int adapter_read(void *ctx, void *file, void *buf, size_t len,
                        size_t *read_len)
{
    (void)ctx;
    return fffs_read((struct fffs_file *)file, buf, len, read_len);
}

static int adapter_fstat(void *ctx, void *file, uint32_t *size)
{
    (void)ctx;
    struct fffs_stat st;
    int rc = fffs_fstat((struct fffs_file *)file, &st);
    if (rc == FFFS_OK) {
        *size = st.size;
    }
    return rc;
}

static int adapter_close(void *ctx, void *file)
{
    fastffs_adapter_t *a = ctx;
    int rc = fffs_close((struct fffs_file *)file);
    for (size_t i = 0; i < FASTFFS_BENCHFS_MAX_OPEN_FILES; ++i) {
        if (file == &a->files[i]) {
            a->file_used[i] = false;
            break;
        }
    }
    return rc;
}

static int adapter_delete_file(void *ctx, const char *name)
{
    fastffs_adapter_t *a = ctx;
    return fffs_delete_file(&a->fs, name);
}

static int adapter_exists(void *ctx, const char *name, bool *exists)
{
    fastffs_adapter_t *a = ctx;
    return fffs_exists(&a->fs, name, exists);
}

static int adapter_list_count(void *ctx, size_t *count)
{
    fastffs_adapter_t *a = ctx;
    return fffs_list(&a->fs, NULL, 0, count);
}

static int adapter_fsinfo(void *ctx, benchfs_info_t *info)
{
    fastffs_adapter_t *a = ctx;
    struct fffs_fsinfo fi;
    int rc = fffs_fsinfo(&a->fs, &fi,
                         FFFS_FSINFO_REFRESH_IF_NEEDED |
                         FFFS_FSINFO_ESTIMATE_METADATA);
    if (rc != FFFS_OK) {
        return rc;
    }
    info->total_valid = (fi.valid_flags & FFFS_FSINFO_TOTAL_VALID) != 0;
    info->used_valid = (fi.valid_flags & FFFS_FSINFO_COMMITTED_BYTES_VALID) != 0;
    info->file_count_valid =
        (fi.valid_flags & FFFS_FSINFO_COMMITTED_FILES_VALID) != 0;
    info->metadata_valid =
        (fi.valid_flags & FFFS_FSINFO_METADATA_ESTIMATE_VALID) != 0;
    info->total_bytes = fi.total_bytes;
    info->used_bytes = fi.committed_data_bytes +
        (info->metadata_valid ? fi.estimated_metadata_bytes : 0u);
    info->file_count = fi.committed_file_count;
    info->metadata_bytes = fi.estimated_metadata_bytes;
    return FFFS_OK;
}

static uint32_t churn_gc_step_budget(fastffs_adapter_t *a,
                                     benchfs_churn_event_t event,
                                     uint32_t file_size)
{
#if FASTFFS_CHURN_GC_POLICY == FASTFFS_CHURN_GC_POLICY_NONE
    (void)a;
    (void)event;
    (void)file_size;
    return 0;
#elif FASTFFS_CHURN_GC_POLICY == FASTFFS_CHURN_GC_POLICY_FIXED
    (void)a;
    (void)event;
    (void)file_size;
    return FASTFFS_CHURN_FIXED_GC_STEPS;
#elif FASTFFS_CHURN_GC_POLICY == FASTFFS_CHURN_GC_POLICY_DEBT
    if (event == BENCHFS_CHURN_EVENT_DELETE) {
        uint32_t sectors = sectors_for_payload(file_size);
        a->gc_reclaim_debt += sectors;
        a->gc_scan_debt += sectors * FASTFFS_CHURN_DEBT_SCAN_MULTIPLIER;
    }
    uint32_t pending = a->gc_reclaim_debt + a->gc_scan_debt;
    if (pending == 0) {
        return 0;
    }
    return pending < FASTFFS_CHURN_DEBT_MAX_STEPS ?
        pending : FASTFFS_CHURN_DEBT_MAX_STEPS;
#else
#error "Unsupported FASTFFS_CHURN_GC_POLICY"
#endif
}

static int adapter_run_gc(void *ctx, benchfs_churn_event_t event,
                          uint32_t file_size, benchfs_gc_stats_t *stats)
{
    fastffs_adapter_t *a = ctx;
    uint32_t steps = churn_gc_step_budget(a, event, file_size);
    if (steps == 0) {
        return FFFS_OK;
    }
    int64_t t0 = esp_timer_get_time();
    for (uint32_t i = 0; i < steps; ++i) {
        enum fffs_gc_action action = FFFS_GC_IDLE;
        int rc = fffs_gc_step(&a->fs, &action);
        stats->steps++;
        if (rc != FFFS_OK) {
            stats->errors++;
            stats->time_us = esp_timer_get_time() - t0;
            return rc;
        }
        switch (action) {
        case FFFS_GC_IDLE:
            stats->idle++;
            break;
        case FFFS_GC_SCANNED:
            stats->scanned++;
            break;
        case FFFS_GC_TOMBSTONED:
            stats->tombstoned++;
            break;
        case FFFS_GC_ERASED:
            stats->erased++;
            break;
        }
    }
    stats->time_us = esp_timer_get_time() - t0;
#if FASTFFS_CHURN_GC_POLICY == FASTFFS_CHURN_GC_POLICY_DEBT
    if (stats->erased >= a->gc_reclaim_debt) {
        a->gc_reclaim_debt = 0;
    } else {
        a->gc_reclaim_debt -= stats->erased;
    }
    uint32_t scan_progress = stats->scanned + stats->tombstoned;
    if (scan_progress >= a->gc_scan_debt) {
        a->gc_scan_debt = 0;
    } else {
        a->gc_scan_debt -= scan_progress;
    }
    ESP_LOGI(TAG, "churn gc debt reclaim=%lu scan=%lu",
             (unsigned long)a->gc_reclaim_debt,
             (unsigned long)a->gc_scan_debt);
#endif
    return FFFS_OK;
}

static void adapter_log_config(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG,
             "config index_cache_mode=%d index_heads=%u scratch=%u file_write_buffer=%u alloc_map_mode=%d alloc_map_words=%u gc_policy=%s fixed_steps=%d debt_max_steps=%d debt_scan_multiplier=%d",
             FFFS_INDEX_CACHE_MODE, FASTFFS_INDEX_HEADS,
             FASTFFS_SCRATCH_SIZE, FFFS_FILE_WRITE_BUFFER,
             FFFS_ALLOC_MAP_MODE,
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
             FASTFFS_ALLOC_MAP_WORDS,
#else
             0,
#endif
             gc_policy_name(), FASTFFS_CHURN_FIXED_GC_STEPS,
             FASTFFS_CHURN_DEBT_MAX_STEPS,
             FASTFFS_CHURN_DEBT_SCAN_MULTIPLIER);
}

static void adapter_log_backend_info(void *ctx, const char *label)
{
    fastffs_adapter_t *a = ctx;
    ESP_LOGI(TAG,
             "%s partition_bytes=%lu index_reserved=%lu sector_size=%lu index_sectors=%u max_file_data=%lu",
             label, (unsigned long)a->part->size,
             (unsigned long)(a->fs.index_sectors * a->fs.sector_size),
             (unsigned long)a->fs.sector_size, a->fs.index_sectors,
             (unsigned long)FASTFFS_SECTOR_DATA_BYTES);
}

static int adapter_memory_info(void *ctx, benchfs_memory_info_t *info)
{
    (void)ctx;
    memset(info, 0, sizeof(*info));
    info->base_valid = true;
    info->base_bytes = sizeof(struct fffs) +
        FFFS_INDEX_CACHE_BYTES(FASTFFS_INDEX_HEADS) + FASTFFS_SCRATCH_SIZE;
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    info->base_bytes += sizeof(uint32_t) * FASTFFS_ALLOC_MAP_WORDS;
#endif
    info->open_file_valid = true;
    info->open_file_bytes = sizeof(struct fffs_file);
    return BENCHFS_OK;
}

static const char *raw_stack_op_name(raw_stack_op_t op)
{
    switch (op) {
    case RAW_STACK_BASELINE:
        return "baseline";
    case RAW_STACK_READ_4:
        return "read_4";
    case RAW_STACK_READ_256:
        return "read_256";
    case RAW_STACK_READ_4096:
        return "read_4096";
    case RAW_STACK_WRITE_256:
        return "write_256";
    case RAW_STACK_ERASE_4096:
        return "erase_4096";
    case RAW_STACK_ERASE_65536:
        return "erase_65536";
    default:
        return "unknown";
    }
}

static void raw_stack_task(void *arg)
{
    raw_stack_task_t *task = arg;
    const esp_partition_t *part = task->part;
    task->err = ESP_OK;

    switch (task->op) {
    case RAW_STACK_BASELINE:
        break;
    case RAW_STACK_READ_4:
        task->err = esp_partition_read(part, 0, s_raw_read_buf, 4);
        break;
    case RAW_STACK_READ_256:
        task->err = esp_partition_read(part, 0, s_raw_read_buf, 256);
        break;
    case RAW_STACK_READ_4096:
        task->err = esp_partition_read(part, 0, s_raw_read_buf,
                                       sizeof(s_raw_read_buf));
        break;
    case RAW_STACK_WRITE_256:
        memset(s_raw_write_buf, 0xa5, 256);
        task->err = esp_partition_write(part, 0, s_raw_write_buf, 256);
        break;
    case RAW_STACK_ERASE_4096:
        task->err = esp_partition_erase_range(part, 0, 4096);
        break;
    case RAW_STACK_ERASE_65536:
        task->err = esp_partition_erase_range(part, 0, 65536);
        break;
    default:
        task->err = ESP_ERR_INVALID_ARG;
        break;
    }

    task->used_bytes = benchfs_esp_current_stack_used_bytes(NULL);
    xTaskNotifyGive(task->waiter);
    vTaskDelete(NULL);
}

static bool run_raw_stack_op(const esp_partition_t *part, raw_stack_op_t op,
                             uint32_t *used_bytes, esp_err_t *err)
{
    raw_stack_task_t task = {
        .part = part,
        .waiter = xTaskGetCurrentTaskHandle(),
        .op = op,
        .err = ESP_FAIL,
    };
    BaseType_t ok = xTaskCreate(raw_stack_task, "raw_stack",
                                CONFIG_ESP_MAIN_TASK_STACK_SIZE, &task,
                                tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        return false;
    }
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    *used_bytes = task.used_bytes;
    *err = task.err;
    return true;
}

static void run_raw_partition_stack_probe(const esp_partition_t *part)
{
    if (part == NULL) {
        return;
    }

    uint32_t baseline = 0;
    esp_err_t err = ESP_OK;
    if (!run_raw_stack_op(part, RAW_STACK_BASELINE, &baseline, &err)) {
        ESP_LOGW(TAG, "raw partition stack probe could not start baseline task");
        return;
    }

    ESP_LOGI(TAG, "raw partition stack baseline used_bytes=%lu",
             (unsigned long)baseline);

    const raw_stack_op_t ops[] = {
        RAW_STACK_READ_4,
        RAW_STACK_READ_256,
        RAW_STACK_READ_4096,
        RAW_STACK_ERASE_4096,
        RAW_STACK_WRITE_256,
        RAW_STACK_ERASE_65536,
    };

    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
        uint32_t used = 0;
        err = ESP_OK;
        if (!run_raw_stack_op(part, ops[i], &used, &err)) {
            ESP_LOGW(TAG, "raw partition stack op=%s could not start task",
                     raw_stack_op_name(ops[i]));
            continue;
        }
        uint32_t over_baseline = used > baseline ? used - baseline : 0;
        ESP_LOGI(TAG,
                 "raw partition stack op=%s err=%s used_bytes=%lu over_baseline=%lu",
                 raw_stack_op_name(ops[i]), esp_err_to_name(err),
                 (unsigned long)used, (unsigned long)over_baseline);
    }
}

void app_main(void)
{
    static fastffs_adapter_t adapter;
    benchfs_config_t cfg;
    benchfs_default_config(&cfg, "FASTFFS ESP32-S3");
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
        .run_gc = adapter_run_gc,
        .memory_info = adapter_memory_info,
        .stack_used_bytes = benchfs_esp_current_stack_used_bytes,
        .run_noop_stack_baseline = benchfs_esp_run_noop_stack_baseline,
        .log_config = adapter_log_config,
        .log_backend_info = adapter_log_backend_info,
    };

    (void)benchfs_run(&cfg, &ops, &adapter);
    (void)adapter_unmount(&adapter);
    run_raw_partition_stack_probe(adapter.part);
}
