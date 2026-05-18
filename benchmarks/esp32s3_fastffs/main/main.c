#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "churn_model.h"
#include "fastffs/fastffs.h"

#define BENCH_TINY_FILES 192
#define BENCH_TINY_SIZE 64
#define BENCH_MED_FILES 16
#define BENCH_MED_SIZE (50 * 1024)
#define BENCH_COLD_TINY_READS 32
#define BENCH_COLD_MED_READS 4
#define CHURN_MAX_FILES 256
#define CHURN_TARGET_LIVE_PERCENT 60
#define CHURN_TARGET_LIVE_BYTES 2308848
#define CHURN_TARGET_WRITTEN_BYTES (8 * 1024 * 1024)
#define CHURN_TARGET_SLACK_BYTES (128 * 1024)
#define CHURN_FORCE_LARGE_AFTER_BYTES (7 * 1024 * 1024)
#define CHURN_SEED 0x4f465346u
#define CHURN_DELETE_LATENCY_SAMPLES 1024
#define FASTFFS_PARTITION_LABEL "storage"

_Static_assert(CHURN_MAX_FILES == BENCH_CHURN_MAX_FILES,
               "churn model and harness slot counts must match");

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
#define FASTFFS_FORCED_GC_STEPS 4096
#define FASTFFS_SECTOR_DATA_BYTES (4096 - 64 - 12)
#define FASTFFS_ALLOC_MAP_WORDS (((4 * 1024 * 1024) / 4096 + 31) / 32)
#define FASTFFS_CHURN_GC_POLICY_FIXED 1
#define FASTFFS_CHURN_GC_POLICY_NONE 2
#define FASTFFS_CHURN_GC_POLICY_DEBT 3
#ifndef FASTFFS_CHURN_GC_POLICY
#define FASTFFS_CHURN_GC_POLICY FASTFFS_CHURN_GC_POLICY_DEBT
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
#ifndef FASTFFS_CHURN_ERASE_BEFORE_FORMAT
#define FASTFFS_CHURN_ERASE_BEFORE_FORMAT 1
#endif
#ifndef FASTFFS_BASELINE_ERASE_BEFORE_FORMAT
#define FASTFFS_BASELINE_ERASE_BEFORE_FORMAT 1
#endif

static const char *TAG = "fastffs_bench";
static uint8_t buf[1024];

typedef enum {
    SIZE_SMALL = 0,
    SIZE_MEDIUM = 1,
    SIZE_LARGE = 2,
    SIZE_CLASS_COUNT = 3,
} size_class_t;

typedef enum {
    CHURN_EVENT_DELETE = 0,
    CHURN_EVENT_WRITE = 1,
} churn_event_t;

typedef struct {
    uint8_t live;
    size_class_t cls;
    uint32_t size;
    char name[24];
} file_slot_t;

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
    uint32_t steps;
    uint32_t idle;
    uint32_t scanned;
    uint32_t tombstoned;
    uint32_t erased;
    uint32_t errors;
    int64_t time_us;
} gc_stats_t;

typedef struct {
    uint32_t ops;
    int64_t total_us;
    int64_t min_us;
    int64_t max_us;
    uint32_t sample_count;
    uint32_t samples_us[CHURN_DELETE_LATENCY_SAMPLES];
} op_time_stats_t;

static const esp_partition_t *s_part;
static struct fffs_backend s_backend;
static struct fffs s_fs;
static uint32_t s_index_cache[
    ((FFFS_INDEX_CACHE_BYTES(FASTFFS_INDEX_HEADS) + sizeof(uint32_t) - 1u) /
     sizeof(uint32_t)) ?
    ((FFFS_INDEX_CACHE_BYTES(FASTFFS_INDEX_HEADS) + sizeof(uint32_t) - 1u) /
     sizeof(uint32_t)) : 1u];
static uint8_t s_scratch[FASTFFS_SCRATCH_SIZE];
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
static uint32_t s_alloc_map[FASTFFS_ALLOC_MAP_WORDS];
#endif
static bool s_mounted;
static file_slot_t churn_files[CHURN_MAX_FILES];
static class_stats_t churn_delete_class_stats[SIZE_CLASS_COUNT];
static int64_t churn_delete_class_max_us[SIZE_CLASS_COUNT];
static op_time_stats_t churn_delete_latency;
static uint32_t churn_latency_sorted[CHURN_DELETE_LATENCY_SAMPLES];
static gc_stats_t gc_total;
static uint32_t gc_reclaim_debt;
static uint32_t gc_scan_debt;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static uint32_t kib_per_s(uint32_t bytes, int64_t elapsed_us)
{
    if (elapsed_us <= 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)bytes * 1000000ULL) /
                      ((uint64_t)elapsed_us * 1024ULL));
}

static uint32_t sectors_for_payload(uint32_t size)
{
    return (size + FASTFFS_SECTOR_DATA_BYTES - 1) / FASTFFS_SECTOR_DATA_BYTES;
}

static const char *churn_gc_policy_name(void)
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

static size_class_t classify_size(uint32_t size)
{
    if (size < 20 * 1024) {
        return SIZE_SMALL;
    }
    if (size < 100 * 1024) {
        return SIZE_MEDIUM;
    }
    return SIZE_LARGE;
}

static const char *class_name(size_class_t cls)
{
    switch (cls) {
    case SIZE_SMALL:
        return "small_10_20k";
    case SIZE_MEDIUM:
        return "medium_20_60k";
    case SIZE_LARGE:
        return "large_350k";
    default:
        return "unknown";
    }
}

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

static int setup_backend(void)
{
    if (s_part != NULL) {
        return 0;
    }
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_ANY,
                                      FASTFFS_PARTITION_LABEL);
    if (s_part == NULL) {
        ESP_LOGE(TAG, "partition '%s' not found", FASTFFS_PARTITION_LABEL);
        return -1;
    }
    s_backend = (struct fffs_backend){
        .ctx = (void *)s_part,
        .size = s_part->size,
        .read_granule = 1,
        .program_granule = 1,
        .read = partition_read,
        .program = partition_program,
        .erase = partition_erase,
    };
    ESP_LOGI(TAG, "using partition '%s' at 0x%lx size 0x%lx",
             s_part->label, (unsigned long)s_part->address,
             (unsigned long)s_part->size);
    return 0;
}

static int mount_fastffs(void)
{
    struct fffs_mount_options opts = {
        .index_cache = s_index_cache,
        .index_cache_size = sizeof(s_index_cache),
        .index_hash_table_size = FASTFFS_INDEX_HEADS,
        .scratch = s_scratch,
        .scratch_size = sizeof(s_scratch),
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
        .alloc_map = s_alloc_map,
        .alloc_map_words = FASTFFS_ALLOC_MAP_WORDS,
#endif
    };
    int rc = fffs_mount(&s_fs, &s_backend, &opts);
    s_mounted = rc == FFFS_OK;
    return rc;
}

static void unmount_fastffs(void)
{
    if (s_mounted) {
        fffs_unmount(&s_fs);
        s_mounted = false;
    }
}

static int format_fastffs(void)
{
    struct fffs_format_options opts = {
        .index_sectors = FFFS_DEFAULT_INDEX_SECTORS,
        .sector_size = FFFS_SECTOR_4K,
    };
    return fffs_format(&s_backend, &opts);
}

static int format_mount_phase(const char *label)
{
    unmount_fastffs();
    int64_t t0 = now_us();
    int rc = format_fastffs();
    ESP_LOGI(TAG, "%s format rc=%s time_us=%lld", label,
             fffs_status_name(rc), (long long)(now_us() - t0));
    if (rc != FFFS_OK) {
        return rc;
    }
    t0 = now_us();
    rc = mount_fastffs();
    ESP_LOGI(TAG, "%s mount rc=%s time_us=%lld", label,
             fffs_status_name(rc), (long long)(now_us() - t0));
    return rc;
}

static int bootstrap_erase_and_remount(const char *label)
{
    unmount_fastffs();
    int64_t t0 = now_us();
    esp_err_t err = esp_partition_erase_range(s_part, 0, s_part->size);
    ESP_LOGI(TAG, "%s bootstrap erase rc=%s time_us=%lld", label,
             esp_err_to_name(err), (long long)(now_us() - t0));
    if (err != ESP_OK) {
        return FFFS_ERR_IO;
    }
    return format_mount_phase(label);
}

static int erase_partition_for_phase(const char *label)
{
    unmount_fastffs();
    int64_t t0 = now_us();
    esp_err_t err = esp_partition_erase_range(s_part, 0, s_part->size);
    ESP_LOGI(TAG, "%s preformat erase rc=%s time_us=%lld", label,
             esp_err_to_name(err), (long long)(now_us() - t0));
    return err == ESP_OK ? FFFS_OK : FFFS_ERR_IO;
}

static void log_gc_stats(const char *label, const gc_stats_t *s)
{
    ESP_LOGI(TAG,
             "%s gc steps=%lu idle=%lu scanned=%lu tombstoned=%lu erased=%lu errors=%lu time_us=%lld",
             label, (unsigned long)s->steps, (unsigned long)s->idle,
             (unsigned long)s->scanned, (unsigned long)s->tombstoned,
             (unsigned long)s->erased, (unsigned long)s->errors,
             (long long)s->time_us);
}

static int run_gc_steps(const char *label, uint32_t steps, gc_stats_t *stats)
{
    gc_stats_t local = {0};
    int64_t t0 = now_us();
    for (uint32_t i = 0; i < steps; ++i) {
        enum fffs_gc_action action = FFFS_GC_IDLE;
        int rc = fffs_gc_step(&s_fs, &action);
        local.steps++;
        if ((i & 0x0f) == 0x0f) {
            vTaskDelay(1);
        }
        if (rc != FFFS_OK) {
            local.errors++;
            local.time_us = now_us() - t0;
            gc_total.steps += local.steps;
            gc_total.errors += local.errors;
            gc_total.time_us += local.time_us;
            if (stats) {
                *stats = local;
            }
            ESP_LOGI(TAG, "%s gc error rc=%s step=%lu", label,
                     fffs_status_name(rc), (unsigned long)i);
            return rc;
        }
        switch (action) {
        case FFFS_GC_IDLE:
            local.idle++;
            break;
        case FFFS_GC_SCANNED:
            local.scanned++;
            break;
        case FFFS_GC_TOMBSTONED:
            local.tombstoned++;
            break;
        case FFFS_GC_ERASED:
            local.erased++;
            break;
        }
    }
    local.time_us = now_us() - t0;
    gc_total.steps += local.steps;
    gc_total.idle += local.idle;
    gc_total.scanned += local.scanned;
    gc_total.tombstoned += local.tombstoned;
    gc_total.erased += local.erased;
    gc_total.errors += local.errors;
    gc_total.time_us += local.time_us;
    if (stats) {
        *stats = local;
    }
    if (steps > 0) {
        log_gc_stats(label, &local);
    }
    return FFFS_OK;
}

static uint32_t churn_gc_step_budget(churn_event_t event, uint32_t file_size)
{
#if FASTFFS_CHURN_GC_POLICY == FASTFFS_CHURN_GC_POLICY_NONE
    (void)event;
    (void)file_size;
    return 0;
#elif FASTFFS_CHURN_GC_POLICY == FASTFFS_CHURN_GC_POLICY_FIXED
    (void)event;
    (void)file_size;
    return FASTFFS_CHURN_FIXED_GC_STEPS;
#elif FASTFFS_CHURN_GC_POLICY == FASTFFS_CHURN_GC_POLICY_DEBT
    if (event == CHURN_EVENT_DELETE) {
        uint32_t sectors = sectors_for_payload(file_size);
        gc_reclaim_debt += sectors;
        gc_scan_debt += sectors * FASTFFS_CHURN_DEBT_SCAN_MULTIPLIER;
    }
    uint32_t pending = gc_reclaim_debt + gc_scan_debt;
    if (pending == 0) {
        return 0;
    }
    return pending < FASTFFS_CHURN_DEBT_MAX_STEPS ?
        pending : FASTFFS_CHURN_DEBT_MAX_STEPS;
#else
#error "Unsupported FASTFFS_CHURN_GC_POLICY"
#endif
}

static int run_churn_gc(churn_event_t event, uint32_t file_size)
{
    uint32_t steps = churn_gc_step_budget(event, file_size);
    if (steps == 0) {
        return FFFS_OK;
    }

    gc_stats_t local = {0};
    int rc = run_gc_steps("churn idle", steps, &local);
#if FASTFFS_CHURN_GC_POLICY == FASTFFS_CHURN_GC_POLICY_DEBT
    uint32_t erased = local.erased;
    if (erased >= gc_reclaim_debt) {
        gc_reclaim_debt = 0;
    } else {
        gc_reclaim_debt -= erased;
    }

    uint32_t scan_progress = local.scanned + local.tombstoned;
    if (scan_progress >= gc_scan_debt) {
        gc_scan_debt = 0;
    } else {
        gc_scan_debt -= scan_progress;
    }
    ESP_LOGI(TAG, "churn gc debt reclaim=%lu scan=%lu",
             (unsigned long)gc_reclaim_debt, (unsigned long)gc_scan_debt);
#endif
    return rc;
}

static void record_op_time(op_time_stats_t *stats, int64_t elapsed_us)
{
    stats->ops++;
    stats->total_us += elapsed_us;
    if (stats->ops == 1 || elapsed_us < stats->min_us) {
        stats->min_us = elapsed_us;
    }
    if (elapsed_us > stats->max_us) {
        stats->max_us = elapsed_us;
    }
    if (stats->sample_count < CHURN_DELETE_LATENCY_SAMPLES) {
        stats->samples_us[stats->sample_count++] = (uint32_t)elapsed_us;
    }
}

static void log_op_time_stats(const char *label, const op_time_stats_t *stats)
{
    uint32_t avg_us = stats->ops == 0 ? 0 :
        (uint32_t)(stats->total_us / stats->ops);
    uint32_t n = stats->sample_count;
    for (uint32_t i = 0; i < n; ++i) {
        churn_latency_sorted[i] = stats->samples_us[i];
        uint32_t j = i;
        while (j > 0 && churn_latency_sorted[j - 1] > churn_latency_sorted[j]) {
            uint32_t tmp = churn_latency_sorted[j - 1];
            churn_latency_sorted[j - 1] = churn_latency_sorted[j];
            churn_latency_sorted[j] = tmp;
            j--;
        }
    }
    uint32_t p50 = n == 0 ? 0 : churn_latency_sorted[(50u * (n - 1u) + 50u) / 100u];
    uint32_t p95 = n == 0 ? 0 : churn_latency_sorted[(95u * (n - 1u) + 50u) / 100u];
    uint32_t p99 = n == 0 ? 0 : churn_latency_sorted[(99u * (n - 1u) + 50u) / 100u];
    ESP_LOGI(TAG, "%s ops=%lu total_us=%lld avg_us=%lu min_us=%lld p50_us=%lu p95_us=%lu p99_us=%lu max_us=%lld samples=%lu",
             label, (unsigned long)stats->ops,
             (long long)stats->total_us, (unsigned long)avg_us,
             (long long)stats->min_us, (unsigned long)p50,
             (unsigned long)p95, (unsigned long)p99,
             (long long)stats->max_us, (unsigned long)n);
}

static void record_delete_stats(class_stats_t stats[SIZE_CLASS_COUNT],
                                int64_t max_us[SIZE_CLASS_COUNT],
                                op_time_stats_t *latency,
                                size_class_t cls, uint32_t size,
                                int64_t elapsed_us)
{
    stats[cls].ops++;
    stats[cls].files++;
    stats[cls].bytes += size;
    stats[cls].time_us += elapsed_us;
    if (elapsed_us > max_us[cls]) {
        max_us[cls] = elapsed_us;
    }
    record_op_time(latency, elapsed_us);
}

static void log_delete_class_stats(const char *prefix,
                                   const class_stats_t stats[SIZE_CLASS_COUNT],
                                   const int64_t max_us[SIZE_CLASS_COUNT])
{
    for (int i = 0; i < SIZE_CLASS_COUNT; ++i) {
        uint32_t avg_us = stats[i].ops == 0 ? 0 :
            (uint32_t)(stats[i].time_us / stats[i].ops);
        ESP_LOGI(TAG,
                 "%s class=%s ops=%lu files=%lu bytes=%lu time_us=%lld avg_us=%lu max_us=%lld",
                 prefix, class_name((size_class_t)i),
                 (unsigned long)stats[i].ops,
                 (unsigned long)stats[i].files,
                 (unsigned long)stats[i].bytes,
                 (long long)stats[i].time_us,
                 (unsigned long)avg_us,
                 (long long)max_us[i]);
    }
}

static void log_fsinfo(const char *label)
{
    struct fffs_fsinfo info;
    int rc = fffs_fsinfo(&s_fs, &info,
                         FFFS_FSINFO_REFRESH_IF_NEEDED |
                         FFFS_FSINFO_ESTIMATE_METADATA);
    if (rc != FFFS_OK) {
        ESP_LOGI(TAG, "%s fsinfo rc=%s", label, fffs_status_name(rc));
        return;
    }
    ESP_LOGI(TAG,
             "%s fsinfo valid=0x%lx total=%lu committed_files=%lu committed_bytes=%lu inflight_files=%lu inflight_bytes=%lu estimated_metadata=%lu",
             label, (unsigned long)info.valid_flags,
             (unsigned long)info.total_bytes,
             (unsigned long)info.committed_file_count,
             (unsigned long)info.committed_data_bytes,
             (unsigned long)info.inflight_file_count,
             (unsigned long)info.inflight_data_bytes,
             (unsigned long)info.estimated_metadata_bytes);
}

static int bench_write_file_once(const char *name, uint32_t size,
                                 uint32_t seed)
{
    struct fffs_file f;
    int rc = fffs_open(&s_fs, &f, name,
                       FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC);
    if (rc != FFFS_OK) {
        return rc;
    }
    uint32_t done = 0;
    while (done < size) {
        size_t chunk = size - done;
        if (chunk > sizeof(buf)) {
            chunk = sizeof(buf);
        }
        fill_pattern(buf, chunk, seed + done);
        size_t written = 0;
        rc = fffs_write(&f, buf, chunk, &written);
        if (rc != FFFS_OK || written != chunk) {
            (void)fffs_close(&f);
            return rc == FFFS_OK ? FFFS_ERR_IO : rc;
        }
        done += (uint32_t)written;
    }
    return fffs_close(&f);
}

static int bench_write_file(const char *name, uint32_t size, uint32_t seed)
{
    return bench_write_file_once(name, size, seed);
}

static uint32_t bench_read_file_timed(const char *name, read_stats_t *stats)
{
    struct fffs_file f;
    struct fffs_stat st;
    int64_t total_start = now_us();
    int64_t t0 = now_us();
    int rc = fffs_open(&s_fs, &f, name, FFFS_O_RDONLY);
    int64_t open_us = now_us() - t0;
    if (rc != FFFS_OK) {
        ESP_LOGE(TAG, "open read %s failed rc=%s", name, fffs_status_name(rc));
        return 0;
    }
    rc = fffs_fstat(&f, &st);
    if (rc != FFFS_OK) {
        ESP_LOGE(TAG, "fstat %s failed rc=%s", name, fffs_status_name(rc));
        (void)fffs_close(&f);
        return 0;
    }
    uint32_t total = 0;
    int64_t read_us = 0;
    while (total < st.size) {
        size_t want = st.size - total;
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }
        size_t rd = 0;
        t0 = now_us();
        rc = fffs_read(&f, buf, want, &rd);
        read_us += now_us() - t0;
        if (rc != FFFS_OK) {
            ESP_LOGE(TAG, "read %s failed rc=%s", name, fffs_status_name(rc));
            break;
        }
        if (rd == 0) {
            break;
        }
        total += (uint32_t)rd;
    }
    t0 = now_us();
    rc = fffs_close(&f);
    int64_t close_us = now_us() - t0;
    if (rc != FFFS_OK) {
        ESP_LOGE(TAG, "close read %s failed rc=%s", name, fffs_status_name(rc));
    }
    if (stats) {
        stats->files++;
        stats->bytes += total;
        stats->open_us += open_us;
        stats->read_us += read_us;
        stats->close_us += close_us;
        stats->total_us += now_us() - total_start;
    }
    return total;
}

static uint32_t bench_read_file(const char *name)
{
    return bench_read_file_timed(name, NULL);
}

static int bench_delete_file(const char *name)
{
    return fffs_delete_file(&s_fs, name);
}

static void bench_list(void)
{
    int64_t t0 = now_us();
    size_t count = 0;
    int rc = fffs_list(&s_fs, NULL, 0, &count);
    ESP_LOGI(TAG, "list: entries=%lu rc=%s time_us=%lld",
             (unsigned long)count, fffs_status_name(rc),
             (long long)(now_us() - t0));
}

static void log_read_stats(const char *label, const read_stats_t *s)
{
    ESP_LOGI(TAG,
             "%s files=%lu bytes=%lu total_us=%lld total_kib_s=%lu open_us=%lld read_us=%lld read_kib_s=%lu close_us=%lld",
             label, (unsigned long)s->files, (unsigned long)s->bytes,
             (long long)s->total_us,
             (unsigned long)kib_per_s(s->bytes, s->total_us),
             (long long)s->open_us, (long long)s->read_us,
             (unsigned long)kib_per_s(s->bytes, s->read_us),
             (long long)s->close_us);
}

static void log_class_stats(const char *prefix, const class_stats_t stats[SIZE_CLASS_COUNT])
{
    for (int i = 0; i < SIZE_CLASS_COUNT; ++i) {
        ESP_LOGI(TAG,
                 "%s class=%s ops=%lu files=%lu bytes=%lu time_us=%lld kib_s=%lu",
                 prefix, class_name((size_class_t)i),
                 (unsigned long)stats[i].ops,
                 (unsigned long)stats[i].files,
                 (unsigned long)stats[i].bytes,
                 (long long)stats[i].time_us,
                 (unsigned long)kib_per_s(stats[i].bytes, stats[i].time_us));
    }
}

static void log_read_class_stats(const char *prefix,
                                 const read_stats_t stats[SIZE_CLASS_COUNT])
{
    for (int i = 0; i < SIZE_CLASS_COUNT; ++i) {
        char label[80];
        snprintf(label, sizeof(label), "%s class=%s", prefix,
                 class_name((size_class_t)i));
        log_read_stats(label, &stats[i]);
    }
}

static void probe_exists_name(const char *name, exists_stats_t *stats)
{
    bool exists = false;
    int64_t t0 = now_us();
    int rc = fffs_exists(&s_fs, name, &exists);
    stats->total_us += now_us() - t0;
    stats->probes++;
    if (rc == FFFS_OK && exists) {
        stats->found++;
    } else {
        stats->missing++;
    }
}

static void log_exists_stats(const char *label, const exists_stats_t *s)
{
    ESP_LOGI(TAG,
             "%s probes=%lu found=%lu missing=%lu total_us=%lld avg_us=%lld",
             label, (unsigned long)s->probes, (unsigned long)s->found,
             (unsigned long)s->missing, (long long)s->total_us,
             (long long)(s->probes ? s->total_us / s->probes : 0));
}

static void bench_tiny_position_stats(void)
{
    uint32_t bytes;
    int64_t t0;
    t0 = now_us();
    bytes = 0;
    for (int i = 0; i < 32; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "t%03d.bin", i);
        bytes += bench_read_file(name);
    }
    ESP_LOGI(TAG, "read tiny early index files=32 size=64 bytes=%lu time_us=%lld kib_s=%lu",
             (unsigned long)bytes, (long long)(now_us() - t0),
             (unsigned long)kib_per_s(bytes, now_us() - t0));

    t0 = now_us();
    bytes = 0;
    for (int i = 80; i < 112; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "t%03d.bin", i);
        bytes += bench_read_file(name);
    }
    ESP_LOGI(TAG, "read tiny middle index files=32 size=64 bytes=%lu time_us=%lld kib_s=%lu",
             (unsigned long)bytes, (long long)(now_us() - t0),
             (unsigned long)kib_per_s(bytes, now_us() - t0));

    t0 = now_us();
    bytes = 0;
    for (int i = 160; i < 192; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "t%03d.bin", i);
        bytes += bench_read_file(name);
    }
    ESP_LOGI(TAG, "read tiny late index files=32 size=64 bytes=%lu time_us=%lld kib_s=%lu",
             (unsigned long)bytes, (long long)(now_us() - t0),
             (unsigned long)kib_per_s(bytes, now_us() - t0));
}

static void bench_exists_baseline(void)
{
    exists_stats_t tiny_existing = {0};
    exists_stats_t tiny_missing = {0};
    exists_stats_t med_existing = {0};
    exists_stats_t med_missing = {0};
    for (int i = 0; i < 64; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "t%03d.bin", permuted_index(i, BENCH_TINY_FILES, 37, 5));
        probe_exists_name(name, &tiny_existing);
        snprintf(name, sizeof(name), "x%03d.bin", i);
        probe_exists_name(name, &tiny_missing);
    }
    for (int i = 0; i < BENCH_MED_FILES; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "m%03d.bin", permuted_index(i, BENCH_MED_FILES, 5, 1));
        probe_exists_name(name, &med_existing);
        snprintf(name, sizeof(name), "z%03d.bin", i);
        probe_exists_name(name, &med_missing);
    }
    log_exists_stats("exists baseline tiny existing", &tiny_existing);
    log_exists_stats("exists baseline tiny missing", &tiny_missing);
    log_exists_stats("exists baseline medium existing", &med_existing);
    log_exists_stats("exists baseline medium missing", &med_missing);
}

static void bench_cold_start_phase(void)
{
    unmount_fastffs();
    int64_t t0 = now_us();
    int rc = mount_fastffs();
    ESP_LOGI(TAG, "cold normal mount rc=%s time_us=%lld",
             fffs_status_name(rc), (long long)(now_us() - t0));
    bench_list();
    read_stats_t tiny = {0};
    read_stats_t med = {0};
    for (int i = 0; i < BENCH_COLD_TINY_READS; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "t%03d.bin",
                 permuted_index(i, BENCH_TINY_FILES, 37, 11));
        (void)bench_read_file_timed(name, &tiny);
    }
    log_read_stats("cold read tiny split", &tiny);
    for (int i = 0; i < BENCH_COLD_MED_READS; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "m%03d.bin",
                 permuted_index(i, BENCH_MED_FILES, 5, 2));
        (void)bench_read_file_timed(name, &med);
    }
    log_read_stats("cold read medium split", &med);
}

static void log_live_distribution(void)
{
    live_dist_t dist[SIZE_CLASS_COUNT] = {0};
    uint32_t total_files = 0;
    uint32_t total_bytes = 0;
    for (int i = 0; i < CHURN_MAX_FILES; ++i) {
        if (!churn_files[i].live) {
            continue;
        }
        size_class_t cls = churn_files[i].cls;
        live_dist_t *d = &dist[cls];
        if (d->files == 0 || churn_files[i].size < d->min_size) {
            d->min_size = churn_files[i].size;
        }
        if (churn_files[i].size > d->max_size) {
            d->max_size = churn_files[i].size;
        }
        d->files++;
        d->bytes += churn_files[i].size;
        total_files++;
        total_bytes += churn_files[i].size;
    }
    ESP_LOGI(TAG, "churn live distribution total_files=%lu total_bytes=%lu avg_size=%lu",
             (unsigned long)total_files, (unsigned long)total_bytes,
             (unsigned long)(total_files ? total_bytes / total_files : 0));
    for (int i = 0; i < SIZE_CLASS_COUNT; ++i) {
        live_dist_t *d = &dist[i];
        ESP_LOGI(TAG, "churn live class=%s files=%lu bytes=%lu avg_size=%lu min_size=%lu max_size=%lu",
                 class_name((size_class_t)i), (unsigned long)d->files,
                 (unsigned long)d->bytes,
                 (unsigned long)(d->files ? d->bytes / d->files : 0),
                 (unsigned long)d->min_size, (unsigned long)d->max_size);
    }
}

static void run_churn_cold_reads(void)
{
    unmount_fastffs();
    int64_t t0 = now_us();
    int rc = mount_fastffs();
    ESP_LOGI(TAG, "churn cold mount rc=%s time_us=%lld",
             fffs_status_name(rc), (long long)(now_us() - t0));
    bench_list();
    read_stats_t read_stats[SIZE_CLASS_COUNT] = {0};
    int sampled[SIZE_CLASS_COUNT] = {0};
    int limits[SIZE_CLASS_COUNT] = {12, 6, 1};
    for (int pass = 0; pass < CHURN_MAX_FILES; ++pass) {
        int i = permuted_index(pass, CHURN_MAX_FILES, 73, 19);
        if (!churn_files[i].live) {
            continue;
        }
        size_class_t cls = churn_files[i].cls;
        if (sampled[cls] >= limits[cls]) {
            continue;
        }
        (void)bench_read_file_timed(churn_files[i].name, &read_stats[cls]);
        sampled[cls]++;
    }
    log_read_class_stats("churn cold read split", read_stats);
    exists_stats_t exists_existing = {0};
    exists_stats_t exists_missing = {0};
    for (int i = 0; i < CHURN_MAX_FILES && exists_existing.probes < 32; ++i) {
        if (churn_files[i].live) {
            probe_exists_name(churn_files[i].name, &exists_existing);
        }
    }
    for (int i = 0; i < 32; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "missing%03d.bin", i);
        probe_exists_name(name, &exists_missing);
    }
    log_exists_stats("exists churn cold existing", &exists_existing);
    log_exists_stats("exists churn cold missing", &exists_missing);
}

static void run_churn_workload(void)
{
    uint32_t creates = 0;
    uint32_t replaces = 0;
    uint32_t deletes = 0;
    uint32_t op = 0;
    static bench_churn_model_t model;
    class_stats_t write_stats[SIZE_CLASS_COUNT] = {0};
    class_stats_t create_write_stats[SIZE_CLASS_COUNT] = {0};
    class_stats_t replace_write_stats[SIZE_CLASS_COUNT] = {0};

    ESP_LOGI(TAG, "churn format start");
#if FASTFFS_CHURN_ERASE_BEFORE_FORMAT
    if (erase_partition_for_phase("churn") != FFFS_OK) {
        return;
    }
#endif
    if (format_mount_phase("churn") != FFFS_OK) {
        return;
    }
    memset(churn_files, 0, sizeof(churn_files));
    memset(&gc_total, 0, sizeof(gc_total));
    memset(churn_delete_class_stats, 0, sizeof(churn_delete_class_stats));
    memset(churn_delete_class_max_us, 0, sizeof(churn_delete_class_max_us));
    memset(&churn_delete_latency, 0, sizeof(churn_delete_latency));
    gc_reclaim_debt = 0;
    gc_scan_debt = 0;
    bench_churn_model_init(&model, CHURN_SEED, CHURN_TARGET_LIVE_BYTES,
                           CHURN_TARGET_WRITTEN_BYTES,
                           CHURN_TARGET_SLACK_BYTES,
                           CHURN_FORCE_LARGE_AFTER_BYTES);
    ESP_LOGI(TAG, "churn target live_percent=%d target_live_bytes=%lu fixed_live_bytes=%lu slack_bytes=%lu written_target=%lu gc_policy=%s fixed_steps=%d debt_max_steps=%d debt_scan_multiplier=%d erase_before_format=%d",
             CHURN_TARGET_LIVE_PERCENT, (unsigned long)((s_part->size * CHURN_TARGET_LIVE_PERCENT) / 100),
             (unsigned long)CHURN_TARGET_LIVE_BYTES,
             (unsigned long)CHURN_TARGET_SLACK_BYTES,
             (unsigned long)CHURN_TARGET_WRITTEN_BYTES,
             churn_gc_policy_name(), FASTFFS_CHURN_FIXED_GC_STEPS,
             FASTFFS_CHURN_DEBT_MAX_STEPS,
             FASTFFS_CHURN_DEBT_SCAN_MULTIPLIER,
             FASTFFS_CHURN_ERASE_BEFORE_FORMAT);

    while (1) {
        bench_churn_event_t event;
        bench_churn_event_type_t type = bench_churn_model_next(&model, &event);
        if (type == BENCH_CHURN_EVENT_DONE) {
            break;
        }
        if (type == BENCH_CHURN_EVENT_NO_SLOT) {
            ESP_LOGE(TAG, "churn no slot available");
            break;
        }

        if (type == BENCH_CHURN_EVENT_DELETE) {
            int64_t dt = now_us();
            int rc = bench_delete_file(event.name);
            int64_t elapsed = now_us() - dt;
            if (rc != FFFS_OK) {
                ESP_LOGE(TAG, "churn delete failed name=%s rc=%s",
                         event.name, fffs_status_name(rc));
                break;
            }
            record_delete_stats(churn_delete_class_stats,
                                churn_delete_class_max_us,
                                &churn_delete_latency, (size_class_t)event.cls,
                                event.size, elapsed);
            bench_churn_model_apply(&model, &event);
            churn_files[event.slot].live = 0;
            deletes++;
            ESP_LOGI(TAG, "churn delete name=%s time_us=%lld live_bytes=%lu",
                     event.name, (long long)elapsed,
                     (unsigned long)model.live_bytes);
            (void)run_churn_gc(CHURN_EVENT_DELETE, event.size);
            continue;
        }

        int64_t wt = now_us();
        int rc = bench_write_file(event.name, event.size, event.write_seed);
        int64_t write_us = now_us() - wt;
        if (rc == FFFS_ERR_NO_SPACE) {
            ESP_LOGI(TAG, "churn write no_space name=%s size=%lu; forced gc start",
                     event.name, (unsigned long)event.size);
            gc_stats_t forced = {0};
            int gc_rc = run_gc_steps("forced", FASTFFS_FORCED_GC_STEPS,
                                     &forced);
            log_gc_stats("forced summary", &forced);
            if (gc_rc == FFFS_OK) {
                wt = now_us();
                rc = bench_write_file(event.name, event.size, event.write_seed);
                write_us = now_us() - wt;
            }
        }
        if (rc != FFFS_OK) {
            ESP_LOGE(TAG, "churn write failed name=%s rc=%s total_written=%lu live_bytes=%lu",
                     event.name, fffs_status_name(rc),
                     (unsigned long)model.total_written,
                     (unsigned long)model.live_bytes);
            break;
        }
        bench_churn_model_apply(&model, &event);
        churn_files[event.slot].live = 1;
        churn_files[event.slot].cls = (size_class_t)event.cls;
        churn_files[event.slot].size = event.size;
        snprintf(churn_files[event.slot].name, sizeof(churn_files[event.slot].name),
                 "%s", event.name);
        op++;
        if (event.replacing) {
            replaces++;
            replace_write_stats[event.cls].ops++;
            replace_write_stats[event.cls].files++;
            replace_write_stats[event.cls].bytes += event.size;
            replace_write_stats[event.cls].time_us += write_us;
        } else {
            creates++;
            create_write_stats[event.cls].ops++;
            create_write_stats[event.cls].files++;
            create_write_stats[event.cls].bytes += event.size;
            create_write_stats[event.cls].time_us += write_us;
        }
        write_stats[event.cls].ops++;
        write_stats[event.cls].files++;
        write_stats[event.cls].bytes += event.size;
        write_stats[event.cls].time_us += write_us;
        ESP_LOGI(TAG, "churn op=%lu name=%s class=%s size=%lu write_us=%lld write_kib_s=%lu total_written=%lu live=%lu",
                 (unsigned long)op, event.name, class_name((size_class_t)event.cls),
                 (unsigned long)event.size, (long long)write_us,
                 (unsigned long)kib_per_s(event.size, write_us),
                 (unsigned long)model.total_written, (unsigned long)model.live_bytes);
        (void)run_churn_gc(CHURN_EVENT_WRITE, event.size);
    }

    ESP_LOGI(TAG, "churn summary ops=%lu written=%lu live=%lu creates=%lu replaces=%lu deletes=%lu",
             (unsigned long)op, (unsigned long)model.total_written,
             (unsigned long)model.live_bytes, (unsigned long)creates,
             (unsigned long)replaces, (unsigned long)deletes);
    ESP_LOGI(TAG, "churn live files avg=%lu samples=%lu",
             (unsigned long)(model.live_file_samples ?
                 model.live_file_sum / model.live_file_samples : 0),
             (unsigned long)model.live_file_samples);
    log_class_stats("churn write", write_stats);
    log_class_stats("churn create write", create_write_stats);
    log_class_stats("churn replace write", replace_write_stats);
    log_delete_class_stats("churn delete", churn_delete_class_stats,
                           churn_delete_class_max_us);
    log_op_time_stats("churn delete latency", &churn_delete_latency);
    log_gc_stats("churn total", &gc_total);
    ESP_LOGI(TAG, "churn gc final debt reclaim=%lu scan=%lu",
             (unsigned long)gc_reclaim_debt, (unsigned long)gc_scan_debt);
    log_live_distribution();
    bench_list();
    run_churn_cold_reads();
}

static void run_baseline(void)
{
    if (setup_backend() != 0) {
        return;
    }
    if (mount_fastffs() != FFFS_OK) {
        ESP_LOGI(TAG, "initial mount failed; continuing to format");
    } else {
        unmount_fastffs();
    }
#if FASTFFS_BASELINE_ERASE_BEFORE_FORMAT
    if (erase_partition_for_phase("baseline") != FFFS_OK) {
        return;
    }
#endif
    int rc = format_mount_phase("baseline");
    if (rc != FFFS_OK) {
        return;
    }
    ESP_LOGI(TAG, "baseline info partition_bytes=%lu index_reserved=%lu sector_size=%lu index_sectors=%u max_file_data=%lu",
             (unsigned long)s_part->size,
             (unsigned long)(s_fs.index_sectors * s_fs.sector_size),
             (unsigned long)s_fs.sector_size, s_fs.index_sectors,
             (unsigned long)FASTFFS_SECTOR_DATA_BYTES);
    log_fsinfo("baseline empty");

retry_after_bootstrap:
    {
        uint32_t bytes = 0;
        int64_t t0 = now_us();
        for (int i = 0; i < BENCH_TINY_FILES; ++i) {
            char name[24];
            snprintf(name, sizeof(name), "t%03d.bin", i);
            rc = bench_write_file(name, BENCH_TINY_SIZE, (uint32_t)i);
            if (rc == FFFS_ERR_NO_SPACE) {
                ESP_LOGI(TAG, "baseline bootstrap required at tiny index=%d", i);
                if (bootstrap_erase_and_remount("baseline") == FFFS_OK) {
                    goto retry_after_bootstrap;
                }
            }
            if (rc != FFFS_OK) {
                ESP_LOGE(TAG, "write tiny failed i=%d rc=%s", i,
                         fffs_status_name(rc));
                return;
            }
            bytes += BENCH_TINY_SIZE;
        }
        ESP_LOGI(TAG, "write tiny files=%d size=%d bytes=%lu time_us=%lld kib_s=%lu",
                 BENCH_TINY_FILES, BENCH_TINY_SIZE, (unsigned long)bytes,
                 (long long)(now_us() - t0),
                 (unsigned long)kib_per_s(bytes, now_us() - t0));
        log_fsinfo("after tiny");
    }

    bench_list();
    {
        read_stats_t tiny = {0};
        int64_t t0 = now_us();
        uint32_t bytes = 0;
        for (int n = 0; n < BENCH_TINY_FILES; ++n) {
            int i = permuted_index(n, BENCH_TINY_FILES, 37, 5);
            char name[24];
            snprintf(name, sizeof(name), "t%03d.bin", i);
            bytes += bench_read_file_timed(name, &tiny);
        }
        ESP_LOGI(TAG, "read tiny files=%d bytes=%lu time_us=%lld kib_s=%lu",
                 BENCH_TINY_FILES, (unsigned long)bytes,
                 (long long)(now_us() - t0),
                 (unsigned long)kib_per_s(bytes, now_us() - t0));
        log_read_stats("read tiny split", &tiny);
    }

    {
        uint32_t bytes = 0;
        int64_t t0 = now_us();
        for (int i = 0; i < BENCH_MED_FILES; ++i) {
            char name[24];
            snprintf(name, sizeof(name), "m%03d.bin", i);
            rc = bench_write_file(name, BENCH_MED_SIZE, 0x1000u + (uint32_t)i);
            if (rc != FFFS_OK) {
                ESP_LOGE(TAG, "write medium failed i=%d rc=%s", i,
                         fffs_status_name(rc));
                return;
            }
            bytes += BENCH_MED_SIZE;
        }
        ESP_LOGI(TAG, "write medium files=%d size=%d bytes=%lu time_us=%lld kib_s=%lu",
                 BENCH_MED_FILES, BENCH_MED_SIZE, (unsigned long)bytes,
                 (long long)(now_us() - t0),
                 (unsigned long)kib_per_s(bytes, now_us() - t0));
        log_fsinfo("after medium");
    }

    {
        read_stats_t med = {0};
        int64_t t0 = now_us();
        uint32_t bytes = 0;
        for (int n = 0; n < BENCH_MED_FILES; ++n) {
            int i = permuted_index(n, BENCH_MED_FILES, 5, 1);
            char name[24];
            snprintf(name, sizeof(name), "m%03d.bin", i);
            bytes += bench_read_file_timed(name, &med);
        }
        ESP_LOGI(TAG, "read medium files=%d bytes=%lu time_us=%lld kib_s=%lu",
                 BENCH_MED_FILES, (unsigned long)bytes,
                 (long long)(now_us() - t0),
                 (unsigned long)kib_per_s(bytes, now_us() - t0));
        log_read_stats("read medium split", &med);
    }

    bench_list();
    bench_tiny_position_stats();
    bench_exists_baseline();
    bench_cold_start_phase();
    run_churn_workload();
}

void app_main(void)
{
    ESP_LOGI(TAG, "FASTFFS ESP32-S3 benchmark starting");
    ESP_LOGI(TAG, "config index_cache_mode=%d index_heads=%u scratch=%u file_write_buffer=%u alloc_map_mode=%d alloc_map_words=%u",
             FFFS_INDEX_CACHE_MODE, FASTFFS_INDEX_HEADS,
             FASTFFS_SCRATCH_SIZE, FFFS_FILE_WRITE_BUFFER,
             FFFS_ALLOC_MAP_MODE,
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
             FASTFFS_ALLOC_MAP_WORDS
#else
             0
#endif
    );
    ESP_LOGI(TAG, "config baseline_erase_before_format=%d churn_erase_before_format=%d",
             FASTFFS_BASELINE_ERASE_BEFORE_FORMAT,
             FASTFFS_CHURN_ERASE_BEFORE_FORMAT);
    run_baseline();
    ESP_LOGI(TAG, "FASTFFS ESP32-S3 benchmark done");
}
