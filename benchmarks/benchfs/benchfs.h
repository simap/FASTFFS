/*
 * SPDX-License-Identifier: MIT
 *
 * Portable filesystem benchmark runner. Platform and filesystem projects supply
 * an adapter for storage setup, file operations, timing, logging, and optional
 * background work such as FASTFFS GC steps.
 */

#ifndef BENCHFS_H
#define BENCHFS_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BENCHFS_OK 0
#define BENCHFS_ERR_NO_SPACE (-1002)

typedef enum {
    BENCHFS_LOG_ERROR = 0,
    BENCHFS_LOG_INFO = 1,
} benchfs_log_level_t;

typedef enum {
    BENCHFS_OPEN_READ = 0x01,
    BENCHFS_OPEN_WRITE_TRUNC = 0x02,
} benchfs_open_flags_t;

typedef enum {
    BENCHFS_SEEK_SET = 0,
    BENCHFS_SEEK_CUR = 1,
    BENCHFS_SEEK_END = 2,
} benchfs_seek_whence_t;

typedef enum {
    BENCHFS_CHURN_EVENT_DELETE = 0,
    BENCHFS_CHURN_EVENT_WRITE = 1,
} benchfs_churn_event_t;

typedef enum {
    BENCHFS_SIZE_SMALL = 0,
    BENCHFS_SIZE_MEDIUM = 1,
    BENCHFS_SIZE_LARGE = 2,
    BENCHFS_SIZE_CLASS_COUNT = 3,
} benchfs_size_class_t;

typedef struct {
    uint32_t total_bytes;
    uint32_t used_bytes;
    uint32_t file_count;
    uint32_t metadata_bytes;
    bool total_valid;
    bool used_valid;
    bool file_count_valid;
    bool metadata_valid;
} benchfs_info_t;

typedef struct {
    uint32_t steps;
    uint32_t idle;
    uint32_t scanned;
    uint32_t tombstoned;
    uint32_t erased;
    uint32_t errors;
    int64_t time_us;
} benchfs_gc_stats_t;

typedef struct {
    uint32_t base_bytes;
    uint32_t open_file_bytes;
    bool base_valid;
    bool open_file_valid;
} benchfs_memory_info_t;

typedef struct {
    const char *name;
    uint32_t tiny_files;
    uint32_t tiny_size;
    uint32_t medium_files;
    uint32_t medium_size;
    uint32_t cold_tiny_reads;
    uint32_t cold_medium_reads;
    uint32_t churn_target_live_percent;
    uint32_t churn_target_live_bytes;
    uint32_t churn_target_written_bytes;
    uint32_t churn_target_slack_bytes;
    uint32_t churn_force_large_after_bytes;
    uint32_t churn_seed;
    uint32_t churn_delete_latency_samples;
    uint32_t small_churn_max_files;
    uint32_t small_churn_min_size;
    uint32_t small_churn_max_size;
    uint32_t small_churn_target_live_bytes;
    uint32_t small_churn_target_written_bytes;
    uint32_t small_churn_target_slack_bytes;
    uint32_t small_churn_seed;
    uint32_t small_churn_log_every_ops;
    bool erase_before_baseline_format;
    bool erase_before_churn_format;
    bool erase_before_small_churn_format;
} benchfs_config_t;

typedef struct {
    int64_t (*now_us)(void *ctx);
    void (*vlog)(void *ctx, benchfs_log_level_t level, const char *fmt,
                 va_list ap);
    const char *(*error_name)(void *ctx, int rc);
    int (*setup)(void *ctx);
    int (*erase_storage)(void *ctx);
    int (*format)(void *ctx);
    int (*mount)(void *ctx);
    int (*unmount)(void *ctx);
    int (*open)(void *ctx, const char *name, uint32_t flags, void **file);
    int (*write)(void *ctx, void *file, const void *buf, size_t len);
    int (*read)(void *ctx, void *file, void *buf, size_t len, size_t *read_len);
    int (*seek)(void *ctx, void *file, int32_t offset,
                benchfs_seek_whence_t whence, uint32_t *pos);
    int (*fstat)(void *ctx, void *file, uint32_t *size);
    int (*close)(void *ctx, void *file);
    int (*delete_file)(void *ctx, const char *name);
    int (*exists)(void *ctx, const char *name, bool *exists);
    int (*list_count)(void *ctx, size_t *count);
    int (*fsinfo)(void *ctx, benchfs_info_t *info);
    int (*run_gc)(void *ctx, benchfs_churn_event_t event, uint32_t file_size,
                  benchfs_gc_stats_t *stats);
    int (*memory_info)(void *ctx, benchfs_memory_info_t *info);
    uint32_t (*stack_used_bytes)(void *ctx);
    int (*run_noop_stack_baseline)(void *ctx, const benchfs_config_t *cfg,
                                   uint32_t *used_bytes);
    void (*log_config)(void *ctx);
    void (*log_backend_info)(void *ctx, const char *label);
} benchfs_ops_t;

void benchfs_default_config(benchfs_config_t *cfg, const char *name);
int benchfs_run(const benchfs_config_t *cfg, const benchfs_ops_t *ops,
                void *ctx);
int benchfs_run_noop(const benchfs_config_t *cfg,
                     int64_t (*now_us)(void *ctx), void *timer_ctx);

#ifdef __cplusplus
}
#endif

#endif
