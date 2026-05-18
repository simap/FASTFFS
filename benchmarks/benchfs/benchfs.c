/*
 * SPDX-License-Identifier: MIT
 */

#include "benchfs.h"

#include <stdio.h>
#include <string.h>

#include "churn_model.h"

#define BENCHFS_BUF_SIZE 1024
#define BENCHFS_MAX_DELETE_LATENCY_SAMPLES 1024

_Static_assert(BENCH_CHURN_MAX_FILES == 256,
               "benchfs assumes the shared churn model slot count");

typedef struct {
    uint32_t ops;
    uint32_t files;
    uint32_t bytes;
    int64_t time_us;
} class_stats_t;

typedef struct {
    uint32_t files;
    uint32_t bytes;
    int64_t open_us;
    int64_t read_us;
    int64_t close_us;
    int64_t total_us;
} read_stats_t;

typedef struct {
    uint32_t probes;
    uint32_t found;
    uint32_t missing;
    int64_t total_us;
} exists_stats_t;

typedef struct {
    uint32_t files;
    uint32_t bytes;
    uint32_t min_size;
    uint32_t max_size;
} live_dist_t;

typedef struct {
    uint32_t ops;
    int64_t total_us;
    int64_t min_us;
    int64_t max_us;
    uint32_t sample_count;
    uint32_t samples_us[BENCHFS_MAX_DELETE_LATENCY_SAMPLES];
} op_time_stats_t;

typedef struct {
    int64_t model_next_us;
    int64_t model_apply_us;
    int64_t stats_us;
    int64_t log_us;
} overhead_stats_t;

typedef struct {
    const benchfs_config_t *cfg;
    const benchfs_ops_t *ops;
    void *ctx;
    uint8_t buf[BENCHFS_BUF_SIZE];
    bench_churn_model_t churn_model;
    bench_churn_slot_t churn_files[BENCH_CHURN_MAX_FILES];
    class_stats_t churn_delete_class_stats[BENCHFS_SIZE_CLASS_COUNT];
    int64_t churn_delete_class_max_us[BENCHFS_SIZE_CLASS_COUNT];
    op_time_stats_t churn_delete_latency;
    uint32_t churn_latency_sorted[BENCHFS_MAX_DELETE_LATENCY_SAMPLES];
    benchfs_gc_stats_t gc_total;
    overhead_stats_t churn_overhead;
} benchfs_t;

void benchfs_default_config(benchfs_config_t *cfg, const char *name)
{
    *cfg = (benchfs_config_t){
        .name = name,
        .tiny_files = 192,
        .tiny_size = 64,
        .medium_files = 16,
        .medium_size = 50 * 1024,
        .cold_tiny_reads = 32,
        .cold_medium_reads = 4,
        .churn_target_live_percent = 60,
        .churn_target_live_bytes = 2308848,
        .churn_target_written_bytes = 8 * 1024 * 1024,
        .churn_target_slack_bytes = 128 * 1024,
        .churn_force_large_after_bytes = 7 * 1024 * 1024,
        .churn_seed = 0x4f465346u,
        .churn_delete_latency_samples = BENCHFS_MAX_DELETE_LATENCY_SAMPLES,
        .erase_before_baseline_format = true,
        .erase_before_churn_format = true,
    };
}

static int64_t now_us(benchfs_t *b)
{
    return b->ops->now_us(b->ctx);
}

static void blog(benchfs_t *b, benchfs_log_level_t level, const char *fmt, ...)
{
    va_list ap;
    int64_t t0 = now_us(b);
    va_start(ap, fmt);
    b->ops->vlog(b->ctx, level, fmt, ap);
    va_end(ap);
    b->churn_overhead.log_us += now_us(b) - t0;
}

static const char *err_name(benchfs_t *b, int rc)
{
    if (b->ops->error_name) {
        return b->ops->error_name(b->ctx, rc);
    }
    return rc == BENCHFS_OK ? "OK" : "ERR";
}

static uint64_t bytes_per_s(uint32_t bytes, int64_t elapsed_us)
{
    if (elapsed_us <= 0) {
        return 0;
    }
    return ((uint64_t)bytes * 1000000ULL) / (uint64_t)elapsed_us;
}

static int permuted_index(int i, int count, int stride, int offset)
{
    return (i * stride + offset) % count;
}

static void fill_pattern(uint8_t *dst, size_t len, uint32_t seed)
{
    for (size_t i = 0; i < len; ++i) {
        dst[i] = (uint8_t)(seed + i * 33u + (i >> 3));
    }
}

static const char *class_name(benchfs_size_class_t cls)
{
    switch (cls) {
    case BENCHFS_SIZE_SMALL:
        return "small_10_20k";
    case BENCHFS_SIZE_MEDIUM:
        return "medium_20_60k";
    case BENCHFS_SIZE_LARGE:
        return "large_350k";
    default:
        return "unknown";
    }
}

static int erase_format_mount_phase(benchfs_t *b, const char *label,
                                    bool erase_before_format)
{
    (void)b->ops->unmount(b->ctx);
    if (erase_before_format && b->ops->erase_storage) {
        int64_t t0 = now_us(b);
        int rc = b->ops->erase_storage(b->ctx);
        blog(b, BENCHFS_LOG_INFO, "%s preformat erase rc=%s time_us=%lld",
             label, err_name(b, rc), (long long)(now_us(b) - t0));
        if (rc != BENCHFS_OK) {
            return rc;
        }
    }

    int64_t t0 = now_us(b);
    int rc = b->ops->format(b->ctx);
    blog(b, BENCHFS_LOG_INFO, "%s format rc=%s time_us=%lld", label,
         err_name(b, rc), (long long)(now_us(b) - t0));
    if (rc != BENCHFS_OK) {
        return rc;
    }

    t0 = now_us(b);
    rc = b->ops->mount(b->ctx);
    blog(b, BENCHFS_LOG_INFO, "%s mount rc=%s time_us=%lld", label,
         err_name(b, rc), (long long)(now_us(b) - t0));
    return rc;
}

static int write_file(benchfs_t *b, const char *name, uint32_t size,
                      uint32_t seed)
{
    void *file = NULL;
    int rc = b->ops->open(b->ctx, name, BENCHFS_OPEN_WRITE_TRUNC, &file);
    if (rc != BENCHFS_OK) {
        return rc;
    }
    uint32_t done = 0;
    while (done < size) {
        size_t chunk = size - done;
        if (chunk > sizeof(b->buf)) {
            chunk = sizeof(b->buf);
        }
        fill_pattern(b->buf, chunk, seed + done);
        rc = b->ops->write(b->ctx, file, b->buf, chunk);
        if (rc != BENCHFS_OK) {
            (void)b->ops->close(b->ctx, file);
            return rc;
        }
        done += (uint32_t)chunk;
    }
    return b->ops->close(b->ctx, file);
}

static uint32_t read_file_timed(benchfs_t *b, const char *name,
                                read_stats_t *stats)
{
    void *file = NULL;
    uint32_t size = 0;
    int64_t total_start = now_us(b);
    int64_t t0 = now_us(b);
    int rc = b->ops->open(b->ctx, name, BENCHFS_OPEN_READ, &file);
    int64_t open_us = now_us(b) - t0;
    if (rc != BENCHFS_OK) {
        blog(b, BENCHFS_LOG_ERROR, "open read %s failed rc=%s", name,
             err_name(b, rc));
        return 0;
    }
    rc = b->ops->fstat(b->ctx, file, &size);
    if (rc != BENCHFS_OK) {
        blog(b, BENCHFS_LOG_ERROR, "fstat %s failed rc=%s", name,
             err_name(b, rc));
        (void)b->ops->close(b->ctx, file);
        return 0;
    }

    uint32_t total = 0;
    int64_t read_us = 0;
    while (total < size) {
        size_t want = size - total;
        if (want > sizeof(b->buf)) {
            want = sizeof(b->buf);
        }
        size_t rd = 0;
        t0 = now_us(b);
        rc = b->ops->read(b->ctx, file, b->buf, want, &rd);
        read_us += now_us(b) - t0;
        if (rc != BENCHFS_OK) {
            blog(b, BENCHFS_LOG_ERROR, "read %s failed rc=%s", name,
                 err_name(b, rc));
            break;
        }
        if (rd == 0) {
            break;
        }
        total += (uint32_t)rd;
    }
    t0 = now_us(b);
    rc = b->ops->close(b->ctx, file);
    int64_t close_us = now_us(b) - t0;
    if (rc != BENCHFS_OK) {
        blog(b, BENCHFS_LOG_ERROR, "close read %s failed rc=%s", name,
             err_name(b, rc));
    }
    if (stats) {
        stats->files++;
        stats->bytes += total;
        stats->open_us += open_us;
        stats->read_us += read_us;
        stats->close_us += close_us;
        stats->total_us += now_us(b) - total_start;
    }
    return total;
}

static void log_read_stats(benchfs_t *b, const char *label,
                           const read_stats_t *s)
{
    blog(b, BENCHFS_LOG_INFO,
         "%s files=%lu bytes=%lu total_us=%lld total_bytes_per_s=%llu open_us=%lld read_us=%lld read_bytes_per_s=%llu close_us=%lld",
         label, (unsigned long)s->files, (unsigned long)s->bytes,
         (long long)s->total_us,
         (unsigned long long)bytes_per_s(s->bytes, s->total_us),
         (long long)s->open_us, (long long)s->read_us,
         (unsigned long long)bytes_per_s(s->bytes, s->read_us),
         (long long)s->close_us);
}

static void log_class_stats(benchfs_t *b, const char *prefix,
                            const class_stats_t stats[BENCHFS_SIZE_CLASS_COUNT])
{
    for (int i = 0; i < BENCHFS_SIZE_CLASS_COUNT; ++i) {
        blog(b, BENCHFS_LOG_INFO,
             "%s class=%s ops=%lu files=%lu bytes=%lu time_us=%lld bytes_per_s=%llu",
             prefix, class_name((benchfs_size_class_t)i),
             (unsigned long)stats[i].ops, (unsigned long)stats[i].files,
             (unsigned long)stats[i].bytes, (long long)stats[i].time_us,
             (unsigned long long)bytes_per_s(stats[i].bytes,
                                             stats[i].time_us));
    }
}

static void log_read_class_stats(benchfs_t *b, const char *prefix,
                                 const read_stats_t stats[BENCHFS_SIZE_CLASS_COUNT])
{
    for (int i = 0; i < BENCHFS_SIZE_CLASS_COUNT; ++i) {
        char label[80];
        snprintf(label, sizeof(label), "%s class=%s", prefix,
                 class_name((benchfs_size_class_t)i));
        log_read_stats(b, label, &stats[i]);
    }
}

static int64_t class_stats_time_total(
    const class_stats_t stats[BENCHFS_SIZE_CLASS_COUNT])
{
    int64_t total = 0;
    for (int i = 0; i < BENCHFS_SIZE_CLASS_COUNT; ++i) {
        total += stats[i].time_us;
    }
    return total;
}

static void record_op_time(op_time_stats_t *stats, int64_t elapsed_us,
                           uint32_t sample_limit)
{
    stats->ops++;
    stats->total_us += elapsed_us;
    if (stats->ops == 1 || elapsed_us < stats->min_us) {
        stats->min_us = elapsed_us;
    }
    if (elapsed_us > stats->max_us) {
        stats->max_us = elapsed_us;
    }
    if (stats->sample_count < sample_limit &&
        stats->sample_count < BENCHFS_MAX_DELETE_LATENCY_SAMPLES) {
        stats->samples_us[stats->sample_count++] = (uint32_t)elapsed_us;
    }
}

static void log_op_time_stats(benchfs_t *b, const char *label,
                              const op_time_stats_t *stats)
{
    uint32_t avg_us = stats->ops == 0 ? 0 :
        (uint32_t)(stats->total_us / stats->ops);
    uint32_t n = stats->sample_count;
    for (uint32_t i = 0; i < n; ++i) {
        b->churn_latency_sorted[i] = stats->samples_us[i];
        uint32_t j = i;
        while (j > 0 &&
               b->churn_latency_sorted[j - 1] > b->churn_latency_sorted[j]) {
            uint32_t tmp = b->churn_latency_sorted[j - 1];
            b->churn_latency_sorted[j - 1] = b->churn_latency_sorted[j];
            b->churn_latency_sorted[j] = tmp;
            j--;
        }
    }
    uint32_t p50 = n == 0 ? 0 : b->churn_latency_sorted[(50u * (n - 1u) + 50u) / 100u];
    uint32_t p95 = n == 0 ? 0 : b->churn_latency_sorted[(95u * (n - 1u) + 50u) / 100u];
    uint32_t p99 = n == 0 ? 0 : b->churn_latency_sorted[(99u * (n - 1u) + 50u) / 100u];
    blog(b, BENCHFS_LOG_INFO,
         "%s ops=%lu total_us=%lld avg_us=%lu min_us=%lld p50_us=%lu p95_us=%lu p99_us=%lu max_us=%lld samples=%lu",
         label, (unsigned long)stats->ops, (long long)stats->total_us,
         (unsigned long)avg_us, (long long)stats->min_us,
         (unsigned long)p50, (unsigned long)p95, (unsigned long)p99,
         (long long)stats->max_us, (unsigned long)n);
}

static void record_delete_stats(benchfs_t *b, benchfs_size_class_t cls,
                                uint32_t size, int64_t elapsed_us)
{
    class_stats_t *stats = b->churn_delete_class_stats;
    stats[cls].ops++;
    stats[cls].files++;
    stats[cls].bytes += size;
    stats[cls].time_us += elapsed_us;
    if (elapsed_us > b->churn_delete_class_max_us[cls]) {
        b->churn_delete_class_max_us[cls] = elapsed_us;
    }
    record_op_time(&b->churn_delete_latency, elapsed_us,
                   b->cfg->churn_delete_latency_samples);
}

static void log_delete_class_stats(benchfs_t *b, const char *prefix)
{
    for (int i = 0; i < BENCHFS_SIZE_CLASS_COUNT; ++i) {
        const class_stats_t *s = &b->churn_delete_class_stats[i];
        uint32_t avg_us = s->ops == 0 ? 0 : (uint32_t)(s->time_us / s->ops);
        blog(b, BENCHFS_LOG_INFO,
             "%s class=%s ops=%lu files=%lu bytes=%lu time_us=%lld avg_us=%lu max_us=%lld",
             prefix, class_name((benchfs_size_class_t)i),
             (unsigned long)s->ops, (unsigned long)s->files,
             (unsigned long)s->bytes, (long long)s->time_us,
             (unsigned long)avg_us,
             (long long)b->churn_delete_class_max_us[i]);
    }
}

static void log_gc_stats(benchfs_t *b, const char *label,
                         const benchfs_gc_stats_t *s)
{
    blog(b, BENCHFS_LOG_INFO,
         "%s gc steps=%lu idle=%lu scanned=%lu tombstoned=%lu erased=%lu errors=%lu time_us=%lld",
         label, (unsigned long)s->steps, (unsigned long)s->idle,
         (unsigned long)s->scanned, (unsigned long)s->tombstoned,
         (unsigned long)s->erased, (unsigned long)s->errors,
         (long long)s->time_us);
}

static void add_gc_stats(benchfs_gc_stats_t *dst, const benchfs_gc_stats_t *src)
{
    dst->steps += src->steps;
    dst->idle += src->idle;
    dst->scanned += src->scanned;
    dst->tombstoned += src->tombstoned;
    dst->erased += src->erased;
    dst->errors += src->errors;
    dst->time_us += src->time_us;
}

static int run_churn_gc(benchfs_t *b, benchfs_churn_event_t event,
                        uint32_t file_size)
{
    if (!b->ops->run_gc) {
        return BENCHFS_OK;
    }
    benchfs_gc_stats_t local = {0};
    int rc = b->ops->run_gc(b->ctx, event, file_size, &local);
    add_gc_stats(&b->gc_total, &local);
    if (local.steps > 0) {
        log_gc_stats(b, "churn idle", &local);
    }
    return rc;
}

static void bench_list(benchfs_t *b)
{
    size_t count = 0;
    int64_t t0 = now_us(b);
    int rc = b->ops->list_count(b->ctx, &count);
    blog(b, BENCHFS_LOG_INFO, "list: entries=%lu rc=%s time_us=%lld",
         (unsigned long)count, err_name(b, rc), (long long)(now_us(b) - t0));
}

static bool read_fsinfo(benchfs_t *b, benchfs_info_t *info)
{
    if (!b->ops->fsinfo) {
        return false;
    }
    memset(info, 0, sizeof(*info));
    int rc = b->ops->fsinfo(b->ctx, info);
    if (rc != BENCHFS_OK) {
        return false;
    }
    return true;
}

static void log_fsinfo(benchfs_t *b, const char *label,
                       const benchfs_info_t *info)
{
    benchfs_info_t local;
    if (!info) {
        if (!read_fsinfo(b, &local)) {
            blog(b, BENCHFS_LOG_INFO, "%s fsinfo unavailable", label);
            return;
        }
        info = &local;
    }
    blog(b, BENCHFS_LOG_INFO,
         "%s fsinfo total_valid=%d used_valid=%d file_count_valid=%d metadata_valid=%d total=%lu used=%lu files=%lu metadata=%lu",
         label, info->total_valid, info->used_valid, info->file_count_valid,
         info->metadata_valid, (unsigned long)info->total_bytes,
         (unsigned long)info->used_bytes, (unsigned long)info->file_count,
         (unsigned long)info->metadata_bytes);
}

static void log_storage_overhead(benchfs_t *b, const char *label,
                                 uint32_t files, uint32_t payload_bytes,
                                 const benchfs_info_t *baseline,
                                 const benchfs_info_t *current)
{
    if (!baseline || !current || !baseline->used_valid ||
        !current->used_valid) {
        return;
    }
    uint32_t used_delta = current->used_bytes >= baseline->used_bytes ?
        current->used_bytes - baseline->used_bytes : 0;
    uint32_t overhead = used_delta > payload_bytes ?
        used_delta - payload_bytes : 0;
    blog(b, BENCHFS_LOG_INFO,
         "%s storage_overhead files=%lu payload_bytes=%lu used_delta=%lu bytes_per_file=%lu overhead_per_file=%lu overhead_pct=%lu",
         label, (unsigned long)files, (unsigned long)payload_bytes,
         (unsigned long)used_delta,
         (unsigned long)(files ? used_delta / files : 0),
         (unsigned long)(files ? overhead / files : 0),
         (unsigned long)(payload_bytes ?
             ((uint64_t)overhead * 100ULL) / payload_bytes : 0ULL));
}

static void probe_exists_name(benchfs_t *b, const char *name,
                              exists_stats_t *stats)
{
    bool exists = false;
    int64_t t0 = now_us(b);
    int rc = b->ops->exists(b->ctx, name, &exists);
    stats->total_us += now_us(b) - t0;
    stats->probes++;
    if (rc == BENCHFS_OK && exists) {
        stats->found++;
    } else {
        stats->missing++;
    }
}

static void log_exists_stats(benchfs_t *b, const char *label,
                             const exists_stats_t *s)
{
    blog(b, BENCHFS_LOG_INFO,
         "%s probes=%lu found=%lu missing=%lu total_us=%lld avg_us=%lld",
         label, (unsigned long)s->probes, (unsigned long)s->found,
         (unsigned long)s->missing, (long long)s->total_us,
         (long long)(s->probes ? s->total_us / s->probes : 0));
}

static void bench_tiny_position_stats(benchfs_t *b)
{
    const struct {
        const char *label;
        int start;
        int end;
    } ranges[] = {
        {"read tiny early index", 0, 32},
        {"read tiny middle index", 80, 112},
        {"read tiny late index", 160, 192},
    };
    for (size_t r = 0; r < sizeof(ranges) / sizeof(ranges[0]); ++r) {
        uint32_t bytes = 0;
        read_stats_t split = {0};
        int64_t t0 = now_us(b);
        for (int i = ranges[r].start; i < ranges[r].end; ++i) {
            char name[24];
            snprintf(name, sizeof(name), "t%03d.bin", i);
            bytes += read_file_timed(b, name, &split);
        }
        int64_t elapsed = now_us(b) - t0;
        blog(b, BENCHFS_LOG_INFO,
             "%s files=32 size=64 bytes=%lu time_us=%lld bytes_per_s=%llu",
             ranges[r].label, (unsigned long)bytes, (long long)elapsed,
             (unsigned long long)bytes_per_s(bytes, elapsed));
        char split_label[80];
        snprintf(split_label, sizeof(split_label), "%s split", ranges[r].label);
        log_read_stats(b, split_label, &split);
    }
}

static void bench_exists_baseline(benchfs_t *b)
{
    exists_stats_t tiny_existing = {0};
    exists_stats_t tiny_missing = {0};
    exists_stats_t med_existing = {0};
    exists_stats_t med_missing = {0};
    for (int i = 0; i < 64; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "t%03d.bin",
                 permuted_index(i, b->cfg->tiny_files, 37, 5));
        probe_exists_name(b, name, &tiny_existing);
        snprintf(name, sizeof(name), "x%03d.bin", i);
        probe_exists_name(b, name, &tiny_missing);
    }
    for (uint32_t i = 0; i < b->cfg->medium_files; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "m%03lu.bin",
                 (unsigned long)permuted_index((int)i, b->cfg->medium_files,
                                               5, 1));
        probe_exists_name(b, name, &med_existing);
        snprintf(name, sizeof(name), "z%03lu.bin", (unsigned long)i);
        probe_exists_name(b, name, &med_missing);
    }
    log_exists_stats(b, "exists baseline tiny existing", &tiny_existing);
    log_exists_stats(b, "exists baseline tiny missing", &tiny_missing);
    log_exists_stats(b, "exists baseline medium existing", &med_existing);
    log_exists_stats(b, "exists baseline medium missing", &med_missing);
}

static void bench_cold_start_phase(benchfs_t *b)
{
    (void)b->ops->unmount(b->ctx);
    int64_t t0 = now_us(b);
    int rc = b->ops->mount(b->ctx);
    blog(b, BENCHFS_LOG_INFO, "cold normal mount rc=%s time_us=%lld",
         err_name(b, rc), (long long)(now_us(b) - t0));
    bench_list(b);
    read_stats_t tiny = {0};
    read_stats_t med = {0};
    for (uint32_t i = 0; i < b->cfg->cold_tiny_reads; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "t%03d.bin",
                 permuted_index((int)i, b->cfg->tiny_files, 37, 11));
        (void)read_file_timed(b, name, &tiny);
    }
    log_read_stats(b, "cold read tiny split", &tiny);
    for (uint32_t i = 0; i < b->cfg->cold_medium_reads; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "m%03d.bin",
                 permuted_index((int)i, b->cfg->medium_files, 5, 2));
        (void)read_file_timed(b, name, &med);
    }
    log_read_stats(b, "cold read medium split", &med);
}

static void log_live_distribution(benchfs_t *b)
{
    live_dist_t dist[BENCHFS_SIZE_CLASS_COUNT] = {0};
    uint32_t total_files = 0;
    uint32_t total_bytes = 0;
    for (int i = 0; i < BENCH_CHURN_MAX_FILES; ++i) {
        if (!b->churn_files[i].live) {
            continue;
        }
        benchfs_size_class_t cls =
            (benchfs_size_class_t)b->churn_files[i].cls;
        live_dist_t *d = &dist[cls];
        if (d->files == 0 || b->churn_files[i].size < d->min_size) {
            d->min_size = b->churn_files[i].size;
        }
        if (b->churn_files[i].size > d->max_size) {
            d->max_size = b->churn_files[i].size;
        }
        d->files++;
        d->bytes += b->churn_files[i].size;
        total_files++;
        total_bytes += b->churn_files[i].size;
    }
    blog(b, BENCHFS_LOG_INFO,
         "churn live distribution total_files=%lu total_bytes=%lu avg_size=%lu",
         (unsigned long)total_files, (unsigned long)total_bytes,
         (unsigned long)(total_files ? total_bytes / total_files : 0));
    for (int i = 0; i < BENCHFS_SIZE_CLASS_COUNT; ++i) {
        live_dist_t *d = &dist[i];
        blog(b, BENCHFS_LOG_INFO,
             "churn live class=%s files=%lu bytes=%lu avg_size=%lu min_size=%lu max_size=%lu",
             class_name((benchfs_size_class_t)i), (unsigned long)d->files,
             (unsigned long)d->bytes,
             (unsigned long)(d->files ? d->bytes / d->files : 0),
             (unsigned long)d->min_size, (unsigned long)d->max_size);
    }
}

static void run_churn_cold_reads(benchfs_t *b)
{
    (void)b->ops->unmount(b->ctx);
    int64_t t0 = now_us(b);
    int rc = b->ops->mount(b->ctx);
    blog(b, BENCHFS_LOG_INFO, "churn cold mount rc=%s time_us=%lld",
         err_name(b, rc), (long long)(now_us(b) - t0));
    bench_list(b);
    read_stats_t read_stats[BENCHFS_SIZE_CLASS_COUNT] = {0};
    int sampled[BENCHFS_SIZE_CLASS_COUNT] = {0};
    int limits[BENCHFS_SIZE_CLASS_COUNT] = {12, 6, 1};
    for (int pass = 0; pass < BENCH_CHURN_MAX_FILES; ++pass) {
        int i = permuted_index(pass, BENCH_CHURN_MAX_FILES, 73, 19);
        if (!b->churn_files[i].live) {
            continue;
        }
        benchfs_size_class_t cls =
            (benchfs_size_class_t)b->churn_files[i].cls;
        if (sampled[cls] >= limits[cls]) {
            continue;
        }
        (void)read_file_timed(b, b->churn_files[i].name, &read_stats[cls]);
        sampled[cls]++;
    }
    log_read_class_stats(b, "churn cold read split", read_stats);

    exists_stats_t exists_existing = {0};
    exists_stats_t exists_missing = {0};
    for (int i = 0; i < BENCH_CHURN_MAX_FILES && exists_existing.probes < 32;
         ++i) {
        if (b->churn_files[i].live) {
            probe_exists_name(b, b->churn_files[i].name, &exists_existing);
        }
    }
    for (int i = 0; i < 32; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "missing%03d.bin", i);
        probe_exists_name(b, name, &exists_missing);
    }
    log_exists_stats(b, "exists churn cold existing", &exists_existing);
    log_exists_stats(b, "exists churn cold missing", &exists_missing);
}

static void run_churn_workload(benchfs_t *b)
{
    uint32_t creates = 0;
    uint32_t replaces = 0;
    uint32_t deletes = 0;
    uint32_t op = 0;
    class_stats_t write_stats[BENCHFS_SIZE_CLASS_COUNT] = {0};
    class_stats_t create_write_stats[BENCHFS_SIZE_CLASS_COUNT] = {0};
    class_stats_t replace_write_stats[BENCHFS_SIZE_CLASS_COUNT] = {0};

    blog(b, BENCHFS_LOG_INFO, "churn format start");
    if (erase_format_mount_phase(b, "churn",
                                 b->cfg->erase_before_churn_format) !=
        BENCHFS_OK) {
        return;
    }
    memset(b->churn_files, 0, sizeof(b->churn_files));
    memset(&b->gc_total, 0, sizeof(b->gc_total));
    memset(b->churn_delete_class_stats, 0, sizeof(b->churn_delete_class_stats));
    memset(b->churn_delete_class_max_us, 0,
           sizeof(b->churn_delete_class_max_us));
    memset(&b->churn_delete_latency, 0, sizeof(b->churn_delete_latency));
    memset(&b->churn_overhead, 0, sizeof(b->churn_overhead));
    bench_churn_model_init(&b->churn_model, b->cfg->churn_seed,
                           b->cfg->churn_target_live_bytes,
                           b->cfg->churn_target_written_bytes,
                           b->cfg->churn_target_slack_bytes,
                           b->cfg->churn_force_large_after_bytes);
    blog(b, BENCHFS_LOG_INFO,
         "churn target live_percent=%lu target_live_bytes=%lu fixed_live_bytes=%lu slack_bytes=%lu written_target=%lu erase_before_format=%d",
         (unsigned long)b->cfg->churn_target_live_percent,
         (unsigned long)b->cfg->churn_target_live_bytes,
         (unsigned long)b->cfg->churn_target_live_bytes,
         (unsigned long)b->cfg->churn_target_slack_bytes,
         (unsigned long)b->cfg->churn_target_written_bytes,
         b->cfg->erase_before_churn_format);

    int64_t churn_wall_start_us = now_us(b);
    while (1) {
        bench_churn_event_t event;
        int64_t overhead_t = now_us(b);
        bench_churn_event_type_t type =
            bench_churn_model_next(&b->churn_model, &event);
        b->churn_overhead.model_next_us += now_us(b) - overhead_t;
        if (type == BENCH_CHURN_EVENT_DONE) {
            break;
        }
        if (type == BENCH_CHURN_EVENT_NO_SLOT) {
            blog(b, BENCHFS_LOG_ERROR, "churn no slot available");
            break;
        }

        if (type == BENCH_CHURN_EVENT_DELETE) {
            int64_t dt = now_us(b);
            int rc = b->ops->delete_file(b->ctx, event.name);
            int64_t elapsed = now_us(b) - dt;
            if (rc != BENCHFS_OK) {
                blog(b, BENCHFS_LOG_ERROR,
                     "churn delete failed name=%s rc=%s", event.name,
                     err_name(b, rc));
                break;
            }
            overhead_t = now_us(b);
            record_delete_stats(b, (benchfs_size_class_t)event.cls, event.size,
                                elapsed);
            b->churn_overhead.stats_us += now_us(b) - overhead_t;
            overhead_t = now_us(b);
            bench_churn_model_apply(&b->churn_model, &event);
            b->churn_overhead.model_apply_us += now_us(b) - overhead_t;
            overhead_t = now_us(b);
            b->churn_files[event.slot].live = 0;
            deletes++;
            b->churn_overhead.stats_us += now_us(b) - overhead_t;
            blog(b, BENCHFS_LOG_INFO,
                 "churn delete name=%s time_us=%lld live_bytes=%lu",
                 event.name, (long long)elapsed,
                 (unsigned long)b->churn_model.live_bytes);
            (void)run_churn_gc(b, BENCHFS_CHURN_EVENT_DELETE, event.size);
            continue;
        }

        int64_t wt = now_us(b);
        int rc = write_file(b, event.name, event.size, event.write_seed);
        int64_t write_us = now_us(b) - wt;
        if (rc != BENCHFS_OK) {
            blog(b, BENCHFS_LOG_ERROR,
                 "churn write failed name=%s rc=%s total_written=%lu live_bytes=%lu",
                 event.name, err_name(b, rc),
                 (unsigned long)b->churn_model.total_written,
                 (unsigned long)b->churn_model.live_bytes);
            break;
        }
        overhead_t = now_us(b);
        bench_churn_model_apply(&b->churn_model, &event);
        b->churn_overhead.model_apply_us += now_us(b) - overhead_t;
        overhead_t = now_us(b);
        b->churn_files[event.slot].live = 1;
        b->churn_files[event.slot].cls = event.cls;
        b->churn_files[event.slot].size = event.size;
        snprintf(b->churn_files[event.slot].name,
                 sizeof(b->churn_files[event.slot].name), "%s", event.name);
        op++;
        benchfs_size_class_t cls = (benchfs_size_class_t)event.cls;
        if (event.replacing) {
            replaces++;
            replace_write_stats[cls].ops++;
            replace_write_stats[cls].files++;
            replace_write_stats[cls].bytes += event.size;
            replace_write_stats[cls].time_us += write_us;
        } else {
            creates++;
            create_write_stats[cls].ops++;
            create_write_stats[cls].files++;
            create_write_stats[cls].bytes += event.size;
            create_write_stats[cls].time_us += write_us;
        }
        write_stats[cls].ops++;
        write_stats[cls].files++;
        write_stats[cls].bytes += event.size;
        write_stats[cls].time_us += write_us;
        b->churn_overhead.stats_us += now_us(b) - overhead_t;
        blog(b, BENCHFS_LOG_INFO,
             "churn op=%lu name=%s class=%s size=%lu write_us=%lld write_bytes_per_s=%llu total_written=%lu live=%lu",
             (unsigned long)op, event.name, class_name(cls),
             (unsigned long)event.size, (long long)write_us,
             (unsigned long long)bytes_per_s(event.size, write_us),
             (unsigned long)b->churn_model.total_written,
             (unsigned long)b->churn_model.live_bytes);
        (void)run_churn_gc(b, BENCHFS_CHURN_EVENT_WRITE, event.size);
    }

    int64_t churn_wall_us = now_us(b) - churn_wall_start_us;
    int64_t churn_write_us = class_stats_time_total(write_stats);
    int64_t churn_delete_us = b->churn_delete_latency.total_us;
    int64_t churn_accounted_us =
        churn_write_us + churn_delete_us + b->gc_total.time_us;
    int64_t churn_benchmark_overhead_us =
        churn_wall_us - churn_accounted_us;
    blog(b, BENCHFS_LOG_INFO,
         "churn summary ops=%lu written=%lu live=%lu creates=%lu replaces=%lu deletes=%lu",
         (unsigned long)op, (unsigned long)b->churn_model.total_written,
         (unsigned long)b->churn_model.live_bytes, (unsigned long)creates,
         (unsigned long)replaces, (unsigned long)deletes);
    blog(b, BENCHFS_LOG_INFO,
         "churn accounting wall_us=%lld accounted_us=%lld write_us=%lld delete_us=%lld gc_step_us=%lld benchmark_overhead_us=%lld unaccounted_us=%lld",
         (long long)churn_wall_us, (long long)churn_accounted_us,
         (long long)churn_write_us, (long long)churn_delete_us,
         (long long)b->gc_total.time_us,
         (long long)churn_benchmark_overhead_us,
         (long long)churn_benchmark_overhead_us);
    int64_t measured_overhead_us =
        b->churn_overhead.model_next_us + b->churn_overhead.model_apply_us +
        b->churn_overhead.stats_us + b->churn_overhead.log_us;
    blog(b, BENCHFS_LOG_INFO,
         "churn overhead detail measured_us=%lld model_next_us=%lld model_apply_us=%lld stats_us=%lld log_us=%lld residual_us=%lld",
         (long long)measured_overhead_us,
         (long long)b->churn_overhead.model_next_us,
         (long long)b->churn_overhead.model_apply_us,
         (long long)b->churn_overhead.stats_us,
         (long long)b->churn_overhead.log_us,
         (long long)(churn_benchmark_overhead_us - measured_overhead_us));
    blog(b, BENCHFS_LOG_INFO, "churn live files avg=%lu samples=%lu",
         (unsigned long)(b->churn_model.live_file_samples ?
             b->churn_model.live_file_sum /
                 b->churn_model.live_file_samples : 0),
         (unsigned long)b->churn_model.live_file_samples);
    log_class_stats(b, "churn write", write_stats);
    log_class_stats(b, "churn create write", create_write_stats);
    log_class_stats(b, "churn replace write", replace_write_stats);
    log_delete_class_stats(b, "churn delete");
    log_op_time_stats(b, "churn delete latency", &b->churn_delete_latency);
    log_gc_stats(b, "churn total", &b->gc_total);
    log_live_distribution(b);
    bench_list(b);
    run_churn_cold_reads(b);
}

static void run_baseline(benchfs_t *b)
{
    int rc = b->ops->setup(b->ctx);
    if (rc != BENCHFS_OK) {
        return;
    }
    rc = b->ops->mount(b->ctx);
    if (rc != BENCHFS_OK) {
        blog(b, BENCHFS_LOG_INFO, "initial mount failed; continuing to format");
    } else {
        (void)b->ops->unmount(b->ctx);
    }
    if (erase_format_mount_phase(b, "baseline",
                                 b->cfg->erase_before_baseline_format) !=
        BENCHFS_OK) {
        return;
    }
    if (b->ops->log_backend_info) {
        b->ops->log_backend_info(b->ctx, "baseline info");
    }
    benchfs_info_t baseline_info;
    bool have_baseline_info = read_fsinfo(b, &baseline_info);
    log_fsinfo(b, "baseline empty",
               have_baseline_info ? &baseline_info : NULL);

    uint32_t bytes = 0;
    int64_t t0 = now_us(b);
    for (uint32_t i = 0; i < b->cfg->tiny_files; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "t%03lu.bin", (unsigned long)i);
        rc = write_file(b, name, b->cfg->tiny_size, i);
        if (rc != BENCHFS_OK) {
            blog(b, BENCHFS_LOG_ERROR, "write tiny failed i=%lu rc=%s",
                 (unsigned long)i, err_name(b, rc));
            return;
        }
        bytes += b->cfg->tiny_size;
    }
    int64_t elapsed = now_us(b) - t0;
    blog(b, BENCHFS_LOG_INFO,
         "write tiny files=%lu size=%lu bytes=%lu time_us=%lld bytes_per_s=%llu",
         (unsigned long)b->cfg->tiny_files, (unsigned long)b->cfg->tiny_size,
         (unsigned long)bytes, (long long)elapsed,
         (unsigned long long)bytes_per_s(bytes, elapsed));
    benchfs_info_t tiny_info;
    bool have_tiny_info = read_fsinfo(b, &tiny_info);
    log_fsinfo(b, "after tiny", have_tiny_info ? &tiny_info : NULL);
    log_storage_overhead(b, "after tiny", b->cfg->tiny_files, bytes,
                         have_baseline_info ? &baseline_info : NULL,
                         have_tiny_info ? &tiny_info : NULL);

    bench_list(b);
    read_stats_t tiny = {0};
    t0 = now_us(b);
    bytes = 0;
    for (uint32_t n = 0; n < b->cfg->tiny_files; ++n) {
        int i = permuted_index((int)n, b->cfg->tiny_files, 37, 5);
        char name[24];
        snprintf(name, sizeof(name), "t%03d.bin", i);
        bytes += read_file_timed(b, name, &tiny);
    }
    elapsed = now_us(b) - t0;
    blog(b, BENCHFS_LOG_INFO,
         "read tiny files=%lu bytes=%lu time_us=%lld bytes_per_s=%llu",
         (unsigned long)b->cfg->tiny_files, (unsigned long)bytes,
         (long long)elapsed, (unsigned long long)bytes_per_s(bytes, elapsed));
    log_read_stats(b, "read tiny split", &tiny);

    t0 = now_us(b);
    bytes = 0;
    for (uint32_t i = 0; i < b->cfg->medium_files; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "m%03lu.bin", (unsigned long)i);
        rc = write_file(b, name, b->cfg->medium_size, 0x1000u + i);
        if (rc != BENCHFS_OK) {
            blog(b, BENCHFS_LOG_ERROR, "write medium failed i=%lu rc=%s",
                 (unsigned long)i, err_name(b, rc));
            return;
        }
        bytes += b->cfg->medium_size;
    }
    elapsed = now_us(b) - t0;
    blog(b, BENCHFS_LOG_INFO,
         "write medium files=%lu size=%lu bytes=%lu time_us=%lld bytes_per_s=%llu",
         (unsigned long)b->cfg->medium_files,
         (unsigned long)b->cfg->medium_size, (unsigned long)bytes,
         (long long)elapsed, (unsigned long long)bytes_per_s(bytes, elapsed));
    benchfs_info_t medium_info;
    bool have_medium_info = read_fsinfo(b, &medium_info);
    log_fsinfo(b, "after medium", have_medium_info ? &medium_info : NULL);
    log_storage_overhead(b, "after medium",
                         b->cfg->tiny_files + b->cfg->medium_files,
                         b->cfg->tiny_files * b->cfg->tiny_size + bytes,
                         have_baseline_info ? &baseline_info : NULL,
                         have_medium_info ? &medium_info : NULL);

    read_stats_t med = {0};
    t0 = now_us(b);
    bytes = 0;
    for (uint32_t n = 0; n < b->cfg->medium_files; ++n) {
        int i = permuted_index((int)n, b->cfg->medium_files, 5, 1);
        char name[24];
        snprintf(name, sizeof(name), "m%03d.bin", i);
        bytes += read_file_timed(b, name, &med);
    }
    elapsed = now_us(b) - t0;
    blog(b, BENCHFS_LOG_INFO,
         "read medium files=%lu bytes=%lu time_us=%lld bytes_per_s=%llu",
         (unsigned long)b->cfg->medium_files, (unsigned long)bytes,
         (long long)elapsed, (unsigned long long)bytes_per_s(bytes, elapsed));
    log_read_stats(b, "read medium split", &med);

    bench_list(b);
    bench_tiny_position_stats(b);
    bench_exists_baseline(b);
    bench_cold_start_phase(b);
    run_churn_workload(b);
}

int benchfs_run(const benchfs_config_t *cfg, const benchfs_ops_t *ops,
                void *ctx)
{
    if (!cfg || !ops || !ops->now_us || !ops->vlog || !ops->setup ||
        !ops->format || !ops->mount || !ops->unmount || !ops->open ||
        !ops->write || !ops->read || !ops->fstat || !ops->close ||
        !ops->delete_file || !ops->exists || !ops->list_count) {
        return -1;
    }
    static benchfs_t b;
    memset(&b, 0, sizeof(b));
    b.cfg = cfg;
    b.ops = ops;
    b.ctx = ctx;
    blog(&b, BENCHFS_LOG_INFO, "%s benchmark starting",
         cfg->name ? cfg->name : "filesystem");
    if (ops->log_config) {
        ops->log_config(ctx);
    }
    run_baseline(&b);
    blog(&b, BENCHFS_LOG_INFO, "%s benchmark done",
         cfg->name ? cfg->name : "filesystem");
    return BENCHFS_OK;
}
