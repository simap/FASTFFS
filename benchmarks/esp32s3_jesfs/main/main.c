#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"

#include "churn_model.h"
#include "jesfs.h"

#define BENCH_TINY_FILES 192
#define BENCH_TINY_SIZE  64
#define BENCH_MED_FILES  16
#define BENCH_MED_SIZE   (50 * 1024)
#define BENCH_COLD_TINY_READS 32
#define BENCH_COLD_MED_READS  4
#define CHURN_MAX_FILES 256
#define CHURN_TARGET_LIVE_PERCENT 60
#define CHURN_TARGET_LIVE_BYTES 2308848
#define CHURN_TARGET_WRITTEN_BYTES (8 * 1024 * 1024)
#define CHURN_TARGET_SLACK_BYTES (128 * 1024)
#define CHURN_FORCE_LARGE_AFTER_BYTES (7 * 1024 * 1024)
#define CHURN_SEED 0x4f465346u
#define CHURN_DELETE_LATENCY_SAMPLES 1024
#define JESFS_PARTITION_LABEL "jesfs"

_Static_assert(CHURN_MAX_FILES == BENCH_CHURN_MAX_FILES,
               "churn model and harness slot counts must match");

#ifndef JESFS_BASELINE_ERASE_BEFORE_FORMAT
#define JESFS_BASELINE_ERASE_BEFORE_FORMAT 1
#endif

#ifndef JESFS_CHURN_ERASE_BEFORE_FORMAT
#define JESFS_CHURN_ERASE_BEFORE_FORMAT 1
#endif

static const char *TAG = "jesfs_bench";
static uint8_t buf[1024];
static uint8_t raw4k[4096];

typedef enum {
    SIZE_SMALL = 0,
    SIZE_MEDIUM = 1,
    SIZE_LARGE = 2,
    SIZE_CLASS_COUNT = 3,
} size_class_t;

typedef struct {
    uint8_t live;
    size_class_t cls;
    uint32_t size;
    char name[FNAMELEN + 1];
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
    uint32_t ops;
    int64_t total_us;
    int64_t min_us;
    int64_t max_us;
    uint32_t sample_count;
    uint32_t samples_us[CHURN_DELETE_LATENCY_SAMPLES];
} op_time_stats_t;

static file_slot_t churn_files[CHURN_MAX_FILES];
static class_stats_t churn_delete_stats[SIZE_CLASS_COUNT];
static int64_t churn_delete_max_us[SIZE_CLASS_COUNT];
static op_time_stats_t churn_delete_latency;
static uint32_t churn_latency_sorted[CHURN_DELETE_LATENCY_SAMPLES];

static void fill_pattern(uint8_t *dst, size_t len, uint32_t seed);
static uint32_t bench_read_file_timed(const char *name, read_stats_t *stats);
static void log_read_stats(const char *label, const read_stats_t *stats);
static void log_exists_stats(const char *label, const exists_stats_t *stats);
static void probe_exists_name(const char *name, exists_stats_t *stats);

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static uint64_t bytes_per_s(uint32_t bytes, int64_t elapsed_us)
{
    if (elapsed_us <= 0) {
        return 0;
    }
    return ((uint64_t)bytes * 1000000ULL) / (uint64_t)elapsed_us;
}

static uint32_t jesfs_used_bytes(void)
{
    return sflash_info.total_flash_size > sflash_info.available_disk_size
               ? sflash_info.total_flash_size - sflash_info.available_disk_size
               : 0;
}

static void log_storage_overhead(const char *label, uint32_t files,
                                 uint32_t payload_bytes, uint32_t baseline_used,
                                 uint32_t current_used)
{
    uint32_t used_delta = current_used >= baseline_used ? current_used - baseline_used : 0;
    uint32_t overhead = used_delta > payload_bytes ? used_delta - payload_bytes : 0;
    ESP_LOGI(TAG,
             "%s storage_overhead files=%lu payload_bytes=%lu used_delta=%lu bytes_per_file=%lu overhead_per_file=%lu overhead_pct=%lu",
             label, (unsigned long)files, (unsigned long)payload_bytes,
             (unsigned long)used_delta,
             (unsigned long)(files ? used_delta / files : 0),
             (unsigned long)(files ? overhead / files : 0),
             (unsigned long)(payload_bytes ? ((uint64_t)overhead * 100ULL) / payload_bytes : 0ULL));
}

static int permuted_index(int i, int count, int stride, int offset)
{
    return (i * stride + offset) % count;
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

static const esp_partition_t *find_jesfs_partition(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        JESFS_PARTITION_LABEL);
    if (part == NULL) {
        ESP_LOGE(TAG, "partition '%s' not found", JESFS_PARTITION_LABEL);
    }
    return part;
}

static int erase_partition_for_phase(const char *label)
{
    const esp_partition_t *part = find_jesfs_partition();
    if (part == NULL) {
        return -1;
    }
    (void)fs_deepsleep();
    int64_t t0 = now_us();
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    ESP_LOGI(TAG, "%s preformat erase rc=%s time_us=%lld", label,
             esp_err_to_name(err), (long long)(now_us() - t0));
    if (err != ESP_OK) {
        return -1;
    }

    t0 = now_us();
    int16_t rc = fs_start(FS_START_NORMAL);
    ESP_LOGI(TAG, "%s post-erase wake rc=%d time_us=%lld", label, rc,
             (long long)(now_us() - t0));
    return 0;
}

static void log_raw_result(const char *op, uint32_t bytes, int64_t elapsed_us)
{
    ESP_LOGI(TAG, "raw partition %s bytes=%lu time_us=%lld bytes_per_s=%llu",
             op, (unsigned long)bytes, (long long)elapsed_us,
             (unsigned long long)bytes_per_s(bytes, elapsed_us));
}

static void log_raw_sample_result(const char *op, uint32_t bytes, uint32_t samples,
                                  int64_t min_us, int64_t avg_us, int64_t max_us)
{
    ESP_LOGI(TAG,
             "raw partition %s bytes=%lu samples=%lu min_us=%lld avg_us=%lld max_us=%lld avg_bytes_per_s=%llu",
             op, (unsigned long)bytes, (unsigned long)samples, (long long)min_us,
             (long long)avg_us, (long long)max_us,
             (unsigned long long)bytes_per_s(bytes, avg_us));
}

static void run_raw_partition_bench(void)
{
    const esp_partition_t *part = find_jesfs_partition();
    uint8_t small[256];
    uint8_t reprogram[256];
    int64_t t0;
    esp_err_t err;

    if (part == NULL) {
        return;
    }

    ESP_LOGI(TAG, "raw partition bench start label=%s offset=0x%lx size=0x%lx",
             part->label, (unsigned long)part->address, (unsigned long)part->size);

    fill_pattern(buf, sizeof(buf), 0x51525354u);
    fill_pattern(small, sizeof(small), 0x31323334u);
    for (size_t i = 0; i < sizeof(reprogram); ++i) {
        reprogram[i] = small[i] & 0x0fu;
    }

    t0 = now_us();
    err = esp_partition_erase_range(part, 0, 4096);
    log_raw_result("erase_4k", 4096, now_us() - t0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "raw erase_4k failed: %s", esp_err_to_name(err));
        return;
    }

    int64_t program_min_us = INT64_MAX;
    int64_t program_max_us = 0;
    int64_t program_total_us = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        t0 = now_us();
        err = esp_partition_write(part, i * sizeof(small), small, sizeof(small));
        int64_t elapsed_us = now_us() - t0;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "raw program_256_page failed: %s", esp_err_to_name(err));
            return;
        }
        if (elapsed_us < program_min_us) {
            program_min_us = elapsed_us;
        }
        if (elapsed_us > program_max_us) {
            program_max_us = elapsed_us;
        }
        program_total_us += elapsed_us;
    }
    log_raw_sample_result("program_256_page", sizeof(small), 16, program_min_us,
                          program_total_us / 16, program_max_us);

    int64_t reprogram_min_us = INT64_MAX;
    int64_t reprogram_max_us = 0;
    int64_t reprogram_total_us = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        t0 = now_us();
        err = esp_partition_write(part, i * sizeof(reprogram), reprogram, sizeof(reprogram));
        int64_t elapsed_us = now_us() - t0;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "raw reprogram_256_page failed: %s", esp_err_to_name(err));
            return;
        }
        if (elapsed_us < reprogram_min_us) {
            reprogram_min_us = elapsed_us;
        }
        if (elapsed_us > reprogram_max_us) {
            reprogram_max_us = elapsed_us;
        }
        reprogram_total_us += elapsed_us;
    }
    log_raw_sample_result("reprogram_256_page", sizeof(reprogram), 16, reprogram_min_us,
                          reprogram_total_us / 16, reprogram_max_us);

    t0 = now_us();
    err = esp_partition_read(part, 0, small, 4);
    log_raw_result("read_4", 4, now_us() - t0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "raw read_4 failed: %s", esp_err_to_name(err));
        return;
    }

    t0 = now_us();
    err = esp_partition_read(part, 0, small, sizeof(small));
    log_raw_result("read_256", sizeof(small), now_us() - t0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "raw read_256 failed: %s", esp_err_to_name(err));
        return;
    }

    t0 = now_us();
    err = esp_partition_read(part, 0, raw4k, sizeof(raw4k));
    log_raw_result("read_4k", 4096, now_us() - t0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "raw read_4k failed: %s", esp_err_to_name(err));
        return;
    }

    t0 = now_us();
    err = esp_partition_erase_range(part, 0x10000, 0x10000);
    log_raw_result("erase_64k", 0x10000, now_us() - t0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "raw erase_64k failed: %s", esp_err_to_name(err));
        return;
    }

    t0 = now_us();
    err = esp_partition_erase_range(part, 0x20000, 4096);
    if (err == ESP_OK) {
        for (uint32_t off = 0; off < 4096; off += sizeof(small)) {
            err = esp_partition_write(part, 0x20000 + off, small, sizeof(small));
            if (err != ESP_OK) {
                break;
            }
        }
    }
    log_raw_result("erase_write_4k", 4096, now_us() - t0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "raw erase_write_4k failed: %s", esp_err_to_name(err));
        return;
    }

    t0 = now_us();
    err = esp_partition_erase_range(part, 0x30000, 4096);
    if (err == ESP_OK) {
        for (uint32_t off = 0; off < 4096; off += sizeof(buf)) {
            err = esp_partition_write(part, 0x30000 + off, buf, sizeof(buf));
            if (err != ESP_OK) {
                break;
            }
        }
    }
    log_raw_result("erase_write_4k_1k_chunks", 4096, now_us() - t0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "raw erase_write_4k_1k_chunks failed: %s", esp_err_to_name(err));
    }
}

static void fill_pattern(uint8_t *dst, size_t len, uint32_t seed)
{
    for (size_t i = 0; i < len; ++i) {
        dst[i] = (uint8_t)(seed + i * 33u + (i >> 3));
    }
}

static int bench_write_file(const char *name, uint32_t size, uint32_t seed)
{
    FS_DESC f;
    int16_t rc = fs_open(&f, (char *)name, SF_OPEN_CREATE | SF_OPEN_WRITE);
    if (rc != 0) {
        ESP_LOGE(TAG, "open write %s rc=%d", name, rc);
        return rc;
    }

    uint32_t remaining = size;
    uint32_t written = 0;
    while (remaining > 0) {
        uint32_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        fill_pattern(buf, chunk, seed + written);
        rc = fs_write(&f, buf, chunk);
        if (rc != 0) {
            ESP_LOGE(TAG, "write %s rc=%d at %lu", name, rc, (unsigned long)written);
            break;
        }
        remaining -= chunk;
        written += chunk;
    }

    int16_t close_rc = fs_close(&f);
    if (close_rc != 0) {
        ESP_LOGE(TAG, "close %s rc=%d", name, close_rc);
        return close_rc;
    }
    return rc;
}

static uint32_t bench_read_file(const char *name)
{
    read_stats_t stats = {0};
    return bench_read_file_timed(name, &stats);
}

static uint32_t bench_read_file_timed(const char *name, read_stats_t *stats)
{
    FS_DESC f;
    uint32_t total = 0;
    int64_t t_total = now_us();
    int64_t t0 = now_us();
    int16_t rc = fs_open(&f, (char *)name, SF_OPEN_READ);
    int64_t open_us = now_us() - t0;
    if (rc != 0) {
        ESP_LOGE(TAG, "open read %s rc=%d", name, rc);
        return 0;
    }

    t0 = now_us();
    while (1) {
        int32_t rd = fs_read(&f, buf, sizeof(buf));
        if (rd < 0) {
            ESP_LOGE(TAG, "read %s rc=%ld", name, (long)rd);
            break;
        }
        if (rd == 0) {
            break;
        }
        total += (uint32_t)rd;
    }
    int64_t read_us = now_us() - t0;

    t0 = now_us();
    fs_close(&f);
    int64_t close_us = now_us() - t0;

    if (stats != NULL) {
        stats->files++;
        stats->bytes += total;
        stats->open_us += open_us;
        stats->read_us += read_us;
        stats->close_us += close_us;
        stats->total_us += now_us() - t_total;
    }
    return total;
}

static int bench_delete_file(const char *name)
{
    FS_DESC f;
    int16_t rc = fs_open(&f, (char *)name, SF_OPEN_READ);
    if (rc != 0) {
        ESP_LOGE(TAG, "open delete %s rc=%d", name, rc);
        return rc;
    }
    rc = fs_delete(&f);
    if (rc != 0) {
        ESP_LOGE(TAG, "delete %s rc=%d", name, rc);
    }
    return rc;
}

static void bench_list(void)
{
    FS_STAT st;
    int active = 0;
    int inactive = 0;
    int empty = 0;
    int64_t t0 = now_us();

    for (uint16_t i = 0; i < 1200; ++i) {
        int16_t rc = fs_info(&st, i);
        if (rc == FS_STAT_INDEX) {
            break;
        }
        if (rc == 0) {
            empty++;
        } else if (rc & FS_STAT_ACTIVE) {
            active++;
        } else if (rc & FS_STAT_INACTIVE) {
            inactive++;
        } else if (rc < 0) {
            ESP_LOGE(TAG, "fs_info(%u) rc=%d", i, rc);
            break;
        }
    }

    ESP_LOGI(TAG, "list: active=%d inactive=%d empty=%d time_us=%lld",
             active, inactive, empty, (long long)(now_us() - t0));
}

static void read_named_range(const char *label, char prefix, int start, int count, uint32_t size)
{
    uint32_t bytes = 0;
    int64_t t0 = now_us();

    for (int i = 0; i < count; ++i) {
        char name[FNAMELEN + 1];
        snprintf(name, sizeof(name), "%c%04d.bin", prefix, start + i);
        bytes += bench_read_file(name);
    }

    int64_t elapsed = now_us() - t0;
    ESP_LOGI(TAG, "%s files=%d size=%lu bytes=%lu time_us=%lld bytes_per_s=%llu",
             label, count, (unsigned long)size, (unsigned long)bytes, (long long)elapsed,
             (unsigned long long)bytes_per_s(bytes, elapsed));
}

static void bench_tiny_position_stats(void)
{
    read_named_range("read tiny early index", 't', 0, 32, BENCH_TINY_SIZE);
    read_named_range("read tiny middle index", 't', BENCH_TINY_FILES / 2, 32, BENCH_TINY_SIZE);
    read_named_range("read tiny late index", 't', BENCH_TINY_FILES - 32, 32, BENCH_TINY_SIZE);
}

static void bench_exists_baseline(void)
{
    exists_stats_t existing_tiny = {0};
    exists_stats_t missing_tiny = {0};
    exists_stats_t existing_medium = {0};
    exists_stats_t missing_medium = {0};

    for (int i = 0; i < 64; ++i) {
        char name[FNAMELEN + 1];
        int idx = permuted_index(i, BENCH_TINY_FILES, 73, 19);
        snprintf(name, sizeof(name), "t%04d.bin", idx);
        probe_exists_name(name, &existing_tiny);
    }
    for (int i = 0; i < 64; ++i) {
        char name[FNAMELEN + 1];
        int idx = 3000 + permuted_index(i, BENCH_TINY_FILES, 73, 19);
        snprintf(name, sizeof(name), "x%04d.bin", idx);
        probe_exists_name(name, &missing_tiny);
    }
    for (int i = 0; i < BENCH_MED_FILES; ++i) {
        char name[FNAMELEN + 1];
        int idx = permuted_index(i, BENCH_MED_FILES, 5, 3);
        snprintf(name, sizeof(name), "m%04d.bin", idx);
        probe_exists_name(name, &existing_medium);
    }
    for (int i = 0; i < BENCH_MED_FILES; ++i) {
        char name[FNAMELEN + 1];
        int idx = 4000 + permuted_index(i, BENCH_MED_FILES, 5, 3);
        snprintf(name, sizeof(name), "y%04d.bin", idx);
        probe_exists_name(name, &missing_medium);
    }

    log_exists_stats("exists baseline tiny existing", &existing_tiny);
    log_exists_stats("exists baseline tiny missing", &missing_tiny);
    log_exists_stats("exists baseline medium existing", &existing_medium);
    log_exists_stats("exists baseline medium missing", &missing_medium);
}

static void bench_cold_start_phase(void)
{
    int16_t rc;
    int64_t t0;
    uint32_t bytes = 0;
    read_stats_t tiny_stats = {0};
    read_stats_t med_stats = {0};

    fs_deepsleep();
    memset(&sflash_info, 0, sizeof(sflash_info));

    t0 = now_us();
    rc = fs_start(FS_START_NORMAL);
    ESP_LOGI(TAG, "cold normal mount rc=%d time_us=%lld", rc, (long long)(now_us() - t0));
    if (rc != 0) {
        return;
    }

    bench_list();

    t0 = now_us();
    for (int i = 0; i < BENCH_COLD_TINY_READS; ++i) {
        char name[FNAMELEN + 1];
        int idx = permuted_index(i, BENCH_TINY_FILES, 73, 19);
        snprintf(name, sizeof(name), "t%04d.bin", idx);
        bytes += bench_read_file_timed(name, &tiny_stats);
    }
    int64_t elapsed = now_us() - t0;
    ESP_LOGI(TAG, "cold read tiny files=%d bytes=%lu time_us=%lld bytes_per_s=%llu",
             BENCH_COLD_TINY_READS, (unsigned long)bytes, (long long)elapsed,
             (unsigned long long)bytes_per_s(bytes, elapsed));
    log_read_stats("cold read tiny split", &tiny_stats);

    bytes = 0;
    t0 = now_us();
    for (int i = 0; i < BENCH_COLD_MED_READS; ++i) {
        char name[FNAMELEN + 1];
        int idx = permuted_index(i, BENCH_MED_FILES, 5, 3);
        snprintf(name, sizeof(name), "m%04d.bin", idx);
        bytes += bench_read_file_timed(name, &med_stats);
    }
    elapsed = now_us() - t0;
    ESP_LOGI(TAG, "cold read medium files=%d bytes=%lu time_us=%lld bytes_per_s=%llu",
             BENCH_COLD_MED_READS, (unsigned long)bytes, (long long)elapsed,
             (unsigned long long)bytes_per_s(bytes, elapsed));
    log_read_stats("cold read medium split", &med_stats);
}

static uint32_t count_live_files(void)
{
    uint32_t live_count = 0;
    for (int i = 0; i < CHURN_MAX_FILES; ++i) {
        if (churn_files[i].live) {
            live_count++;
        }
    }
    return live_count;
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

static void log_delete_class_stats(const char *label,
                                   const class_stats_t stats[SIZE_CLASS_COUNT],
                                   const int64_t max_us[SIZE_CLASS_COUNT])
{
    for (int i = 0; i < SIZE_CLASS_COUNT; ++i) {
        uint32_t avg_us = stats[i].ops == 0 ? 0 :
            (uint32_t)(stats[i].time_us / stats[i].ops);
        ESP_LOGI(TAG, "%s class=%s ops=%lu files=%lu bytes=%lu time_us=%lld avg_us=%lu max_us=%lld",
                 label, class_name((size_class_t)i),
                 (unsigned long)stats[i].ops,
                 (unsigned long)stats[i].files,
                 (unsigned long)stats[i].bytes,
                 (long long)stats[i].time_us,
                 (unsigned long)avg_us,
                 (long long)max_us[i]);
    }
}

static void log_class_stats(const char *label, class_stats_t stats[SIZE_CLASS_COUNT])
{
    for (int i = 0; i < SIZE_CLASS_COUNT; ++i) {
        ESP_LOGI(TAG, "%s class=%s ops=%lu files=%lu bytes=%lu time_us=%lld bytes_per_s=%llu",
                 label, class_name((size_class_t)i), (unsigned long)stats[i].ops,
                 (unsigned long)stats[i].files, (unsigned long)stats[i].bytes,
                 (long long)stats[i].time_us,
                 (unsigned long long)bytes_per_s(stats[i].bytes, stats[i].time_us));
    }
}

static int64_t class_stats_time_total(const class_stats_t stats[SIZE_CLASS_COUNT])
{
    int64_t total = 0;
    for (int i = 0; i < SIZE_CLASS_COUNT; ++i) {
        total += stats[i].time_us;
    }
    return total;
}

static void record_class_stats(class_stats_t stats[SIZE_CLASS_COUNT],
                               size_class_t cls, uint32_t size,
                               int64_t elapsed_us)
{
    stats[cls].ops++;
    stats[cls].files++;
    stats[cls].bytes += size;
    stats[cls].time_us += elapsed_us;
}

static void log_read_stats(const char *label, const read_stats_t *stats)
{
    ESP_LOGI(TAG,
             "%s files=%lu bytes=%lu total_us=%lld total_bytes_per_s=%llu open_us=%lld read_us=%lld read_bytes_per_s=%llu close_us=%lld",
             label, (unsigned long)stats->files, (unsigned long)stats->bytes,
             (long long)stats->total_us,
             (unsigned long long)bytes_per_s(stats->bytes, stats->total_us),
             (long long)stats->open_us, (long long)stats->read_us,
             (unsigned long long)bytes_per_s(stats->bytes, stats->read_us),
             (long long)stats->close_us);
}

static void log_read_class_stats(const char *label, read_stats_t stats[SIZE_CLASS_COUNT])
{
    for (int i = 0; i < SIZE_CLASS_COUNT; ++i) {
        char scoped_label[64];
        snprintf(scoped_label, sizeof(scoped_label), "%s class=%s", label, class_name((size_class_t)i));
        log_read_stats(scoped_label, &stats[i]);
    }
}

static void log_exists_stats(const char *label, const exists_stats_t *stats)
{
    ESP_LOGI(TAG, "%s probes=%lu found=%lu missing=%lu total_us=%lld avg_us=%lu",
             label, (unsigned long)stats->probes, (unsigned long)stats->found,
             (unsigned long)stats->missing, (long long)stats->total_us,
             (unsigned long)(stats->probes ? stats->total_us / stats->probes : 0));
}

static void probe_exists_name(const char *name, exists_stats_t *stats)
{
    int64_t t0 = now_us();
    int16_t rc = fs_notexists((char *)name);
    stats->total_us += now_us() - t0;
    stats->probes++;
    if (rc == 0) {
        stats->found++;
    } else {
        stats->missing++;
    }
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
        size_class_t cls = classify_size(churn_files[i].size);
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
                 (unsigned long)(d->files ? d->min_size : 0),
                 (unsigned long)(d->files ? d->max_size : 0));
    }
}

static void run_churn_cold_reads(void)
{
    read_stats_t read_stats[SIZE_CLASS_COUNT] = {0};
    exists_stats_t exists_existing = {0};
    exists_stats_t exists_missing = {0};
    int samples[SIZE_CLASS_COUNT] = {0};
    int wanted[SIZE_CLASS_COUNT] = {12, 6, 2};
    int16_t rc;
    int64_t t0;

    fs_deepsleep();
    memset(&sflash_info, 0, sizeof(sflash_info));

    t0 = now_us();
    rc = fs_start(FS_START_NORMAL);
    ESP_LOGI(TAG, "churn cold mount rc=%d time_us=%lld", rc, (long long)(now_us() - t0));
    if (rc != 0) {
        return;
    }

    bench_list();

    for (int k = 0; k < CHURN_MAX_FILES; ++k) {
        int i = permuted_index(k, CHURN_MAX_FILES, 73, 41);
        if (!churn_files[i].live) {
            continue;
        }
        size_class_t cls = churn_files[i].cls;
        if (samples[cls] >= wanted[cls]) {
            continue;
        }

        (void)bench_read_file_timed(churn_files[i].name, &read_stats[cls]);
        samples[cls]++;
    }

    log_read_class_stats("churn cold read split", read_stats);

    for (int k = 0; k < CHURN_MAX_FILES && exists_existing.probes < 64; ++k) {
        int i = permuted_index(k, CHURN_MAX_FILES, 73, 41);
        if (churn_files[i].live) {
            probe_exists_name(churn_files[i].name, &exists_existing);
        }
    }
    for (int i = 0; i < 64; ++i) {
        char name[FNAMELEN + 1];
        int idx = 5000 + permuted_index(i, CHURN_MAX_FILES, 73, 41);
        snprintf(name, sizeof(name), "z%04d.bin", idx);
        probe_exists_name(name, &exists_missing);
    }
    log_exists_stats("exists churn cold existing", &exists_existing);
    log_exists_stats("exists churn cold missing", &exists_missing);
}

static void run_churn_workload(void)
{
    class_stats_t write_stats[SIZE_CLASS_COUNT] = {0};
    class_stats_t create_write_stats[SIZE_CLASS_COUNT] = {0};
    class_stats_t replace_write_stats[SIZE_CLASS_COUNT] = {0};
    uint32_t create_ops = 0;
    uint32_t replace_ops = 0;
    uint32_t delete_ops = 0;
    uint32_t op = 0;
    uint32_t last_progress_written = 0;
    int64_t last_progress_us = 0;
    static bench_churn_model_t model;
    int16_t rc;
    int64_t t0;

    ESP_LOGI(TAG, "churn format start");
#if JESFS_CHURN_ERASE_BEFORE_FORMAT
    if (erase_partition_for_phase("churn") != 0) {
        return;
    }
#endif
    t0 = now_us();
    rc = fs_format(FS_FORMAT_SOFT);
    ESP_LOGI(TAG, "churn format rc=%d time_us=%lld", rc, (long long)(now_us() - t0));
    if (rc != 0) {
        return;
    }

    t0 = now_us();
    rc = fs_start(FS_START_NORMAL);
    ESP_LOGI(TAG, "churn mount rc=%d time_us=%lld", rc, (long long)(now_us() - t0));
    if (rc != 0) {
        return;
    }

    memset(churn_files, 0, sizeof(churn_files));
    memset(churn_delete_stats, 0, sizeof(churn_delete_stats));
    memset(churn_delete_max_us, 0, sizeof(churn_delete_max_us));
    memset(&churn_delete_latency, 0, sizeof(churn_delete_latency));
    bench_churn_model_init(&model, CHURN_SEED, CHURN_TARGET_LIVE_BYTES,
                           CHURN_TARGET_WRITTEN_BYTES,
                           CHURN_TARGET_SLACK_BYTES,
                           CHURN_FORCE_LARGE_AFTER_BYTES);
    last_progress_us = now_us();

    ESP_LOGI(TAG, "churn target live_percent=%d target_live_bytes=%lu fixed_live_bytes=%lu slack_bytes=%lu written_target=%lu",
             CHURN_TARGET_LIVE_PERCENT, (unsigned long)CHURN_TARGET_LIVE_BYTES,
             (unsigned long)CHURN_TARGET_LIVE_BYTES,
             (unsigned long)CHURN_TARGET_SLACK_BYTES,
             (unsigned long)CHURN_TARGET_WRITTEN_BYTES);

    int64_t churn_wall_start_us = now_us();
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
            int drc = bench_delete_file(event.name);
            int64_t elapsed = now_us() - dt;
            if (drc != 0) {
                ESP_LOGE(TAG, "churn delete failed name=%s rc=%d",
                         event.name, drc);
                break;
            }
            record_delete_stats(churn_delete_stats, churn_delete_max_us,
                                &churn_delete_latency,
                                (size_class_t)event.cls, event.size,
                                elapsed);
            bench_churn_model_apply(&model, &event);
            churn_files[event.slot].live = 0;
            delete_ops++;
            ESP_LOGI(TAG, "churn delete name=%s time_us=%lld live_bytes=%lu",
                     event.name, (long long)elapsed,
                     (unsigned long)model.live_bytes);
            continue;
        }

        t0 = now_us();
        int wrc = bench_write_file(event.name, event.size, event.write_seed);
        int64_t elapsed = now_us() - t0;
        if (wrc != 0) {
            ESP_LOGE(TAG, "churn write failed name=%s rc=%d total_written=%lu live_bytes=%lu",
                     event.name, wrc, (unsigned long)model.total_written,
                     (unsigned long)model.live_bytes);
            break;
        }

        bench_churn_model_apply(&model, &event);
        churn_files[event.slot].live = 1;
        churn_files[event.slot].cls = (size_class_t)event.cls;
        churn_files[event.slot].size = event.size;
        snprintf(churn_files[event.slot].name, sizeof(churn_files[event.slot].name),
                 "%s", event.name);
        if (event.replacing) {
            replace_ops++;
            record_class_stats(replace_write_stats, (size_class_t)event.cls,
                               event.size, elapsed);
        } else {
            create_ops++;
            record_class_stats(create_write_stats, (size_class_t)event.cls,
                               event.size, elapsed);
        }
        record_class_stats(write_stats, (size_class_t)event.cls, event.size,
                           elapsed);
        op++;

        ESP_LOGI(TAG, "churn op=%lu name=%s class=%s size=%lu write_us=%lld write_bytes_per_s=%llu total_written=%lu live=%lu",
                 (unsigned long)op, event.name, class_name((size_class_t)event.cls),
                 (unsigned long)event.size, (long long)elapsed,
                 (unsigned long long)bytes_per_s(event.size, elapsed),
                 (unsigned long)model.total_written, (unsigned long)model.live_bytes);

        if ((op % 25u) == 0u || model.total_written >= CHURN_TARGET_WRITTEN_BYTES) {
            int64_t now = now_us();
            uint32_t interval_bytes = model.total_written - last_progress_written;
            int64_t interval_us = now - last_progress_us;
            uint32_t live_files = count_live_files();
            ESP_LOGI(TAG, "churn progress ops=%lu written=%lu live=%lu creates=%lu replaces=%lu deletes=%lu",
                     (unsigned long)op, (unsigned long)model.total_written,
                     (unsigned long)model.live_bytes,
                     (unsigned long)create_ops, (unsigned long)replace_ops,
                     (unsigned long)delete_ops);
            ESP_LOGI(TAG, "churn progress interval_bytes=%lu interval_us=%lld interval_bytes_per_s=%llu live_files=%lu forced_large=%lu",
                     (unsigned long)interval_bytes, (long long)interval_us,
                     (unsigned long long)bytes_per_s(interval_bytes, interval_us),
                     (unsigned long)live_files, (unsigned long)model.forced_large_written);
            last_progress_written = model.total_written;
            last_progress_us = now;
        }
    }

    int64_t churn_wall_us = now_us() - churn_wall_start_us;
    int64_t churn_write_us = class_stats_time_total(write_stats);
    int64_t churn_delete_us = churn_delete_latency.total_us;
    int64_t churn_accounted_us = churn_write_us + churn_delete_us;
    int64_t churn_benchmark_overhead_us = churn_wall_us - churn_accounted_us;
    ESP_LOGI(TAG, "churn summary ops=%lu written=%lu live=%lu creates=%lu replaces=%lu deletes=%lu",
             (unsigned long)op, (unsigned long)model.total_written,
             (unsigned long)model.live_bytes,
             (unsigned long)create_ops, (unsigned long)replace_ops, (unsigned long)delete_ops);
    ESP_LOGI(TAG, "churn accounting wall_us=%lld accounted_us=%lld write_us=%lld delete_us=%lld gc_step_us=0 benchmark_overhead_us=%lld unaccounted_us=%lld",
             (long long)churn_wall_us, (long long)churn_accounted_us,
             (long long)churn_write_us, (long long)churn_delete_us,
             (long long)churn_benchmark_overhead_us,
             (long long)churn_benchmark_overhead_us);
    ESP_LOGI(TAG, "churn live files avg=%lu samples=%lu",
             (unsigned long)(model.live_file_samples ?
                 model.live_file_sum / model.live_file_samples : 0),
             (unsigned long)model.live_file_samples);
    log_class_stats("churn write", write_stats);
    log_class_stats("churn create write", create_write_stats);
    log_class_stats("churn replace write", replace_write_stats);
    log_delete_class_stats("churn delete", churn_delete_stats,
                           churn_delete_max_us);
    log_op_time_stats("churn delete latency", &churn_delete_latency);
    log_live_distribution();
    bench_list();
    run_churn_cold_reads();
}

static void run_benchmarks(void)
{
    int16_t rc;
    int64_t t0;
    int64_t elapsed;
    uint32_t bytes;
    read_stats_t tiny_read_stats = {0};
    read_stats_t med_read_stats = {0};

    ESP_LOGI(TAG, "config baseline_erase_before_format=%d churn_erase_before_format=%d",
             JESFS_BASELINE_ERASE_BEFORE_FORMAT,
             JESFS_CHURN_ERASE_BEFORE_FORMAT);
    run_raw_partition_bench();

    t0 = now_us();
    rc = fs_start(FS_START_NORMAL);
    ESP_LOGI(TAG, "initial mount rc=%d time_us=%lld", rc, (long long)(now_us() - t0));

#if JESFS_BASELINE_ERASE_BEFORE_FORMAT
    if (erase_partition_for_phase("baseline") != 0) {
        return;
    }
#endif
    t0 = now_us();
    rc = fs_format(FS_FORMAT_SOFT);
    ESP_LOGI(TAG, "format rc=%d time_us=%lld", rc, (long long)(now_us() - t0));
    if (rc != 0) {
        return;
    }

    t0 = now_us();
    rc = fs_start(FS_START_NORMAL);
    ESP_LOGI(TAG, "mount normal rc=%d time_us=%lld", rc, (long long)(now_us() - t0));
    if (rc != 0) {
        return;
    }
    uint32_t baseline_used = jesfs_used_bytes();
    ESP_LOGI(TAG, "baseline overhead_base used=%lu available=%lu total=%lu",
             (unsigned long)baseline_used, (unsigned long)sflash_info.available_disk_size,
             (unsigned long)sflash_info.total_flash_size);

    t0 = now_us();
    for (int i = 0; i < BENCH_TINY_FILES; ++i) {
        char name[FNAMELEN + 1];
        snprintf(name, sizeof(name), "t%04d.bin", i);
        bench_write_file(name, BENCH_TINY_SIZE, (uint32_t)i);
    }
    elapsed = now_us() - t0;
    bytes = BENCH_TINY_FILES * BENCH_TINY_SIZE;
    ESP_LOGI(TAG, "write tiny files=%d size=%d bytes=%lu time_us=%lld bytes_per_s=%llu",
             BENCH_TINY_FILES, BENCH_TINY_SIZE, (unsigned long)bytes, (long long)elapsed,
             (unsigned long long)bytes_per_s(bytes, elapsed));
    log_storage_overhead("after tiny", BENCH_TINY_FILES, bytes, baseline_used,
                         jesfs_used_bytes());

    bench_list();

    t0 = now_us();
    bytes = 0;
    for (int i = 0; i < BENCH_TINY_FILES; ++i) {
        char name[FNAMELEN + 1];
        int idx = permuted_index(i, BENCH_TINY_FILES, 73, 19);
        snprintf(name, sizeof(name), "t%04d.bin", idx);
        bytes += bench_read_file_timed(name, &tiny_read_stats);
    }
    elapsed = now_us() - t0;
    ESP_LOGI(TAG, "read tiny files=%d bytes=%lu time_us=%lld bytes_per_s=%llu",
             BENCH_TINY_FILES, (unsigned long)bytes, (long long)elapsed,
             (unsigned long long)bytes_per_s(bytes, elapsed));
    log_read_stats("read tiny split", &tiny_read_stats);

    t0 = now_us();
    for (int i = 0; i < BENCH_MED_FILES; ++i) {
        char name[FNAMELEN + 1];
        snprintf(name, sizeof(name), "m%04d.bin", i);
        bench_write_file(name, BENCH_MED_SIZE, 0x10000u + (uint32_t)i);
    }
    elapsed = now_us() - t0;
    bytes = BENCH_MED_FILES * BENCH_MED_SIZE;
    ESP_LOGI(TAG, "write medium files=%d size=%d bytes=%lu time_us=%lld bytes_per_s=%llu",
             BENCH_MED_FILES, BENCH_MED_SIZE, (unsigned long)bytes, (long long)elapsed,
             (unsigned long long)bytes_per_s(bytes, elapsed));
    log_storage_overhead("after medium", BENCH_TINY_FILES + BENCH_MED_FILES,
                         BENCH_TINY_FILES * BENCH_TINY_SIZE + bytes,
                         baseline_used, jesfs_used_bytes());

    t0 = now_us();
    bytes = 0;
    for (int i = 0; i < BENCH_MED_FILES; ++i) {
        char name[FNAMELEN + 1];
        int idx = permuted_index(i, BENCH_MED_FILES, 5, 3);
        snprintf(name, sizeof(name), "m%04d.bin", idx);
        bytes += bench_read_file_timed(name, &med_read_stats);
    }
    elapsed = now_us() - t0;
    ESP_LOGI(TAG, "read medium files=%d bytes=%lu time_us=%lld bytes_per_s=%llu",
             BENCH_MED_FILES, (unsigned long)bytes, (long long)elapsed,
             (unsigned long long)bytes_per_s(bytes, elapsed));
    log_read_stats("read medium split", &med_read_stats);

    bench_list();

    bench_tiny_position_stats();
    bench_exists_baseline();

    bench_cold_start_phase();

    run_churn_workload();
}

void app_main(void)
{
    ESP_LOGI(TAG, "JesFS ESP32-S3 benchmark starting");
    run_benchmarks();
    ESP_LOGI(TAG, "JesFS ESP32-S3 benchmark done");
}
