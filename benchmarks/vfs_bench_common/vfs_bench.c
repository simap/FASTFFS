#include "vfs_bench.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef BENCH_TINY_FILES
#define BENCH_TINY_FILES 192
#endif

#define BENCH_TINY_SIZE 64
#define BENCH_MED_FILES 16
#define BENCH_MED_SIZE (50 * 1024)
#define BENCH_COLD_TINY_READS 32
#define BENCH_COLD_MED_READS 4
#define CHURN_MAX_FILES 256
#define CHURN_TARGET_WRITTEN_BYTES (8 * 1024 * 1024)
#define CHURN_TARGET_SLACK_BYTES (128 * 1024)
#define CHURN_FORCE_LARGE_AFTER_BYTES (7 * 1024 * 1024)
#define CHURN_SEED 0x4f465346u
#define WRITE_RETRY_LIMIT 5

#ifndef CHURN_TARGET_LIVE_PERCENT
#define CHURN_TARGET_LIVE_PERCENT 75
#endif

#ifndef CHURN_TARGET_LIVE_BYTES
#define CHURN_TARGET_LIVE_BYTES 0
#endif

static const char *TAG = "vfs_bench";
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
    uint32_t files_attempted;
    uint32_t files_ok;
    uint32_t files_failed;
    uint32_t short_write_events;
    uint32_t zero_write_retries;
    uint32_t zero_write_failures;
    uint32_t flush_retries;
    uint32_t flush_failures;
    uint32_t close_failures;
} write_health_t;

static file_slot_t churn_files[CHURN_MAX_FILES];
static write_health_t write_health;
static uint32_t prng_state = CHURN_SEED;
static int protected_large_slot = -1;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static uint32_t kib_per_s(uint32_t bytes, int64_t elapsed_us)
{
    if (elapsed_us <= 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)bytes * 1000000ULL) / ((uint64_t)elapsed_us * 1024ULL));
}

static uint32_t prng_next(void)
{
    prng_state = prng_state * 1664525u + 1013904223u;
    return prng_state;
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

static void make_path(char *dst, size_t len, const char *name)
{
    snprintf(dst, len, "%s/%s", VFS_BENCH_BASE_PATH, name);
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

static uint32_t choose_churn_size(size_class_t *cls)
{
    uint32_t r = prng_next() % 1000u;
    if (r < 930u) {
        *cls = SIZE_SMALL;
        return (10u * 1024u) + (prng_next() % (10u * 1024u + 1u));
    }
    if (r < 995u) {
        *cls = SIZE_MEDIUM;
        return (20u * 1024u) + (prng_next() % (40u * 1024u + 1u));
    }
    *cls = SIZE_LARGE;
    return 350u * 1024u;
}

static uint32_t forced_large_churn_size(size_class_t *cls)
{
    *cls = SIZE_LARGE;
    return 350u * 1024u;
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

static const esp_partition_t *find_storage_partition(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        VFS_BENCH_PARTITION_LABEL);
    if (part == NULL) {
        ESP_LOGE(TAG, "partition '%s' not found", VFS_BENCH_PARTITION_LABEL);
    }
    return part;
}

static void log_raw_result(const char *op, uint32_t bytes, int64_t elapsed_us)
{
    ESP_LOGI(TAG, "raw partition %s bytes=%lu time_us=%lld kib_s=%lu",
             op, (unsigned long)bytes, (long long)elapsed_us,
             (unsigned long)kib_per_s(bytes, elapsed_us));
}

static void log_raw_sample_result(const char *op, uint32_t bytes, uint32_t samples,
                                  int64_t min_us, int64_t avg_us, int64_t max_us)
{
    ESP_LOGI(TAG,
             "raw partition %s bytes=%lu samples=%lu min_us=%lld avg_us=%lld max_us=%lld avg_kib_s=%lu",
             op, (unsigned long)bytes, (unsigned long)samples, (long long)min_us,
             (long long)avg_us, (long long)max_us,
             (unsigned long)kib_per_s(bytes, avg_us));
}

static void run_raw_partition_bench(void)
{
#ifdef ENABLE_RAW_PARTITION_BENCH
    const esp_partition_t *part = find_storage_partition();
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
#else
    ESP_LOGI(TAG, "raw partition bench skipped");
#endif
}

static size_t log_fs_info(const char *label)
{
    size_t total = 0;
    size_t used = 0;
    esp_err_t err = bench_backend_info(&total, &used);
    ESP_LOGI(TAG, "%s info rc=%s total=%lu used=%lu free=%lu",
             label, esp_err_to_name(err), (unsigned long)total, (unsigned long)used,
             (unsigned long)(total > used ? total - used : 0));
    return err == ESP_OK ? used : 0;
}

static void log_storage_overhead(const char *label, uint32_t files,
                                 uint32_t payload_bytes, size_t baseline_used,
                                 size_t current_used)
{
    size_t used_delta = current_used >= baseline_used ? current_used - baseline_used : 0;
    size_t overhead = used_delta > payload_bytes ? used_delta - payload_bytes : 0;
    ESP_LOGI(TAG,
             "%s storage_overhead files=%lu payload_bytes=%lu used_delta=%lu bytes_per_file=%lu overhead_per_file=%lu overhead_pct=%lu",
             label, (unsigned long)files, (unsigned long)payload_bytes,
             (unsigned long)used_delta,
             (unsigned long)(files ? used_delta / files : 0),
             (unsigned long)(files ? overhead / files : 0),
             (unsigned long)(payload_bytes ? (overhead * 100ULL) / payload_bytes : 0ULL));
}

static void log_write_health(const char *label)
{
    ESP_LOGI(TAG,
             "%s write_health attempted=%lu ok=%lu failed=%lu short_events=%lu zero_retries=%lu zero_failures=%lu flush_retries=%lu flush_failures=%lu close_failures=%lu",
             label, (unsigned long)write_health.files_attempted,
             (unsigned long)write_health.files_ok,
             (unsigned long)write_health.files_failed,
             (unsigned long)write_health.short_write_events,
             (unsigned long)write_health.zero_write_retries,
             (unsigned long)write_health.zero_write_failures,
             (unsigned long)write_health.flush_retries,
             (unsigned long)write_health.flush_failures,
             (unsigned long)write_health.close_failures);
}

static uint32_t current_churn_target_live_bytes(void)
{
    if (CHURN_TARGET_LIVE_BYTES > 0) {
        return CHURN_TARGET_LIVE_BYTES;
    }

    size_t total = 0;
    size_t used = 0;
    esp_err_t err = bench_backend_info(&total, &used);
    if (err != ESP_OK || total == 0) {
        return (3 * 1024 * 1024);
    }
    return (uint32_t)(((uint64_t)total * CHURN_TARGET_LIVE_PERCENT) / 100ULL);
}

static int format_and_mount(const char *label)
{
    int64_t t0;
    esp_err_t err;

    (void)bench_backend_unmount();

    t0 = now_us();
    err = bench_backend_format();
    ESP_LOGI(TAG, "%s format rc=%s time_us=%lld", label, esp_err_to_name(err),
             (long long)(now_us() - t0));
    if (err != ESP_OK) {
        return -1;
    }

    t0 = now_us();
    err = bench_backend_mount(false);
    ESP_LOGI(TAG, "%s mount rc=%s time_us=%lld", label, esp_err_to_name(err),
             (long long)(now_us() - t0));
    if (err != ESP_OK) {
        return -1;
    }
    (void)log_fs_info(label);
    return 0;
}

static int remount_timed(const char *label)
{
    int64_t t0;
    esp_err_t err;

    (void)bench_backend_unmount();
    t0 = now_us();
    err = bench_backend_mount(false);
    ESP_LOGI(TAG, "%s mount rc=%s time_us=%lld", label, esp_err_to_name(err),
             (long long)(now_us() - t0));
    if (err != ESP_OK) {
        return -1;
    }
    log_fs_info(label);
    return 0;
}

static int bench_write_file(const char *name, uint32_t size, uint32_t seed)
{
    char path[48];
    uint32_t file_zero_retries = 0;
    uint32_t file_short_events = 0;
    uint32_t file_flush_retries = 0;
    make_path(path, sizeof(path), name);

    write_health.files_attempted++;

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open write %s errno=%d", path, errno);
        write_health.files_failed++;
        return -1;
    }

    uint32_t remaining = size;
    uint32_t written = 0;
    while (remaining > 0) {
        uint32_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        fill_pattern(buf, chunk, seed + written);
        uint32_t chunk_done = 0;

        while (chunk_done < chunk) {
            errno = 0;
            size_t wr = fwrite(buf + chunk_done, 1, chunk - chunk_done, f);
            if (wr > 0) {
                if (wr < chunk - chunk_done) {
                    file_short_events++;
                    write_health.short_write_events++;
                }
                chunk_done += (uint32_t)wr;
                written += (uint32_t)wr;
                remaining -= (uint32_t)wr;
                continue;
            }

            file_zero_retries++;
            write_health.zero_write_retries++;
            int saved_errno = errno;
            clearerr(f);
            if (file_zero_retries >= WRITE_RETRY_LIMIT) {
                ESP_LOGE(TAG, "write %s stalled wr=0 remaining=%lu errno=%d retries=%lu",
                         path, (unsigned long)(chunk - chunk_done), saved_errno,
                         (unsigned long)file_zero_retries);
                fclose(f);
                write_health.zero_write_failures++;
                write_health.files_failed++;
                return -1;
            }
        }
    }

    for (uint32_t flush_retry = 0; flush_retry <= WRITE_RETRY_LIMIT; ++flush_retry) {
        errno = 0;
        if (fflush(f) == 0) {
            break;
        }
        int saved_errno = errno;
        clearerr(f);
        if (flush_retry == WRITE_RETRY_LIMIT) {
            ESP_LOGE(TAG, "flush %s failed errno=%d retries=%lu", path, saved_errno,
                     (unsigned long)flush_retry);
            fclose(f);
            write_health.flush_failures++;
            write_health.files_failed++;
            return -1;
        }
        file_flush_retries++;
        write_health.flush_retries++;
    }

    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "close write %s errno=%d", path, errno);
        write_health.close_failures++;
        write_health.files_failed++;
        return -1;
    }
    write_health.files_ok++;
    if (file_zero_retries > 0 || file_short_events > 0 || file_flush_retries > 0) {
        ESP_LOGI(TAG, "write retry name=%s short_events=%lu zero_retries=%lu flush_retries=%lu",
                 name, (unsigned long)file_short_events,
                 (unsigned long)file_zero_retries,
                 (unsigned long)file_flush_retries);
    }
    return 0;
}

static uint32_t bench_read_file_timed(const char *name, read_stats_t *stats)
{
    char path[48];
    uint32_t total = 0;
    int64_t t_total = now_us();
    int64_t t0;

    make_path(path, sizeof(path), name);

    t0 = now_us();
    FILE *f = fopen(path, "rb");
    int64_t open_us = now_us() - t0;
    if (f == NULL) {
        ESP_LOGE(TAG, "open read %s errno=%d", path, errno);
        return 0;
    }

    t0 = now_us();
    while (1) {
        size_t rd = fread(buf, 1, sizeof(buf), f);
        total += (uint32_t)rd;
        if (rd < sizeof(buf)) {
            if (ferror(f)) {
                ESP_LOGE(TAG, "read %s errno=%d", path, errno);
            }
            break;
        }
    }
    int64_t read_us = now_us() - t0;

    t0 = now_us();
    fclose(f);
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

static uint32_t bench_read_file(const char *name)
{
    return bench_read_file_timed(name, NULL);
}

static int bench_delete_file(const char *name)
{
    char path[48];
    make_path(path, sizeof(path), name);
    if (unlink(path) != 0) {
        ESP_LOGE(TAG, "delete %s errno=%d", path, errno);
        return -1;
    }
    return 0;
}

static void bench_list(void)
{
    int entries = 0;
    int64_t t0 = now_us();
    DIR *dir = opendir(VFS_BENCH_BASE_PATH);
    if (dir == NULL) {
        ESP_LOGE(TAG, "opendir %s errno=%d", VFS_BENCH_BASE_PATH, errno);
        return;
    }

    while (1) {
        struct dirent *ent = readdir(dir);
        if (ent == NULL) {
            break;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        entries++;
    }
    closedir(dir);

    ESP_LOGI(TAG, "list: entries=%d time_us=%lld", entries, (long long)(now_us() - t0));
}

static void read_named_range(const char *label, char prefix, int start, int count, uint32_t size)
{
    uint32_t bytes = 0;
    int64_t t0 = now_us();

    for (int i = 0; i < count; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "%c%04d.bin", prefix, start + i);
        bytes += bench_read_file(name);
    }

    int64_t elapsed = now_us() - t0;
    ESP_LOGI(TAG, "%s files=%d size=%lu bytes=%lu time_us=%lld kib_s=%lu",
             label, count, (unsigned long)size, (unsigned long)bytes, (long long)elapsed,
             (unsigned long)kib_per_s(bytes, elapsed));
}

static void log_read_stats(const char *label, const read_stats_t *stats)
{
    ESP_LOGI(TAG,
             "%s files=%lu bytes=%lu total_us=%lld total_kib_s=%lu open_us=%lld read_us=%lld read_kib_s=%lu close_us=%lld",
             label, (unsigned long)stats->files, (unsigned long)stats->bytes,
             (long long)stats->total_us,
             (unsigned long)kib_per_s(stats->bytes, stats->total_us),
             (long long)stats->open_us, (long long)stats->read_us,
             (unsigned long)kib_per_s(stats->bytes, stats->read_us),
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

static void log_class_stats(const char *label, class_stats_t stats[SIZE_CLASS_COUNT])
{
    for (int i = 0; i < SIZE_CLASS_COUNT; ++i) {
        ESP_LOGI(TAG, "%s class=%s ops=%lu files=%lu bytes=%lu time_us=%lld kib_s=%lu",
                 label, class_name((size_class_t)i), (unsigned long)stats[i].ops,
                 (unsigned long)stats[i].files, (unsigned long)stats[i].bytes,
                 (long long)stats[i].time_us,
                 (unsigned long)kib_per_s(stats[i].bytes, stats[i].time_us));
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
    char path[48];
    struct stat st;
    make_path(path, sizeof(path), name);

    int64_t t0 = now_us();
    int rc = stat(path, &st);
    stats->total_us += now_us() - t0;
    stats->probes++;
    if (rc == 0) {
        stats->found++;
    } else {
        stats->missing++;
    }
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
        char name[24];
        int idx = permuted_index(i, BENCH_TINY_FILES, 73, 19);
        snprintf(name, sizeof(name), "t%04d.bin", idx);
        probe_exists_name(name, &existing_tiny);
    }
    for (int i = 0; i < 64; ++i) {
        char name[24];
        int idx = 3000 + permuted_index(i, BENCH_TINY_FILES, 73, 19);
        snprintf(name, sizeof(name), "x%04d.bin", idx);
        probe_exists_name(name, &missing_tiny);
    }
    for (int i = 0; i < BENCH_MED_FILES; ++i) {
        char name[24];
        int idx = permuted_index(i, BENCH_MED_FILES, 5, 3);
        snprintf(name, sizeof(name), "m%04d.bin", idx);
        probe_exists_name(name, &existing_medium);
    }
    for (int i = 0; i < BENCH_MED_FILES; ++i) {
        char name[24];
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
    uint32_t bytes = 0;
    read_stats_t tiny_stats = {0};
    read_stats_t med_stats = {0};
    int64_t t0;

    if (remount_timed("cold normal") != 0) {
        return;
    }
    bench_list();

    t0 = now_us();
    for (int i = 0; i < BENCH_COLD_TINY_READS; ++i) {
        char name[24];
        int idx = permuted_index(i, BENCH_TINY_FILES, 73, 19);
        snprintf(name, sizeof(name), "t%04d.bin", idx);
        bytes += bench_read_file_timed(name, &tiny_stats);
    }
    int64_t elapsed = now_us() - t0;
    ESP_LOGI(TAG, "cold read tiny files=%d bytes=%lu time_us=%lld kib_s=%lu",
             BENCH_COLD_TINY_READS, (unsigned long)bytes, (long long)elapsed,
             (unsigned long)kib_per_s(bytes, elapsed));
    log_read_stats("cold read tiny split", &tiny_stats);

    bytes = 0;
    t0 = now_us();
    for (int i = 0; i < BENCH_COLD_MED_READS; ++i) {
        char name[24];
        int idx = permuted_index(i, BENCH_MED_FILES, 5, 3);
        snprintf(name, sizeof(name), "m%04d.bin", idx);
        bytes += bench_read_file_timed(name, &med_stats);
    }
    elapsed = now_us() - t0;
    ESP_LOGI(TAG, "cold read medium files=%d bytes=%lu time_us=%lld kib_s=%lu",
             BENCH_COLD_MED_READS, (unsigned long)bytes, (long long)elapsed,
             (unsigned long)kib_per_s(bytes, elapsed));
    log_read_stats("cold read medium split", &med_stats);
}

static int find_free_slot(void)
{
    for (int i = 0; i < CHURN_MAX_FILES; ++i) {
        if (!churn_files[i].live) {
            return i;
        }
    }
    return -1;
}

static int choose_live_slot(void)
{
    int live_count = 0;
    for (int i = 0; i < CHURN_MAX_FILES; ++i) {
        if (churn_files[i].live && i != protected_large_slot) {
            live_count++;
        }
    }
    if (live_count == 0) {
        return protected_large_slot >= 0 && churn_files[protected_large_slot].live ? protected_large_slot : -1;
    }

    int target = (int)(prng_next() % (uint32_t)live_count);
    for (int i = 0; i < CHURN_MAX_FILES; ++i) {
        if (churn_files[i].live && i != protected_large_slot && target-- == 0) {
            return i;
        }
    }
    return -1;
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

static int delete_slot(int slot, uint32_t *live_bytes, uint32_t *delete_ops)
{
    int64_t t0 = now_us();
    int rc = bench_delete_file(churn_files[slot].name);
    int64_t elapsed = now_us() - t0;

    if (rc == 0) {
        *live_bytes -= churn_files[slot].size;
        churn_files[slot].live = 0;
        (*delete_ops)++;
        ESP_LOGI(TAG, "churn delete name=%s time_us=%lld live_bytes=%lu",
                 churn_files[slot].name, (long long)elapsed, (unsigned long)*live_bytes);
    }
    return rc;
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

    if (remount_timed("churn cold") != 0) {
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
        char name[24];
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
    uint32_t live_bytes = 0;
    uint32_t total_written = 0;
    uint32_t last_progress_written = 0;
    uint32_t target_live_bytes = 0;
    uint32_t create_ops = 0;
    uint32_t replace_ops = 0;
    uint32_t delete_ops = 0;
    uint32_t op = 0;
    uint32_t forced_large_written = 0;
    int64_t last_progress_us = 0;

    ESP_LOGI(TAG, "churn format start");
    if (format_and_mount("churn") != 0) {
        return;
    }

    memset(churn_files, 0, sizeof(churn_files));
    protected_large_slot = -1;
    prng_state = CHURN_SEED;
    target_live_bytes = current_churn_target_live_bytes();
    last_progress_us = now_us();

    ESP_LOGI(TAG, "churn target live_percent=%d target_live_bytes=%lu fixed_live_bytes=%lu slack_bytes=%lu written_target=%lu",
             CHURN_TARGET_LIVE_PERCENT, (unsigned long)target_live_bytes,
             (unsigned long)CHURN_TARGET_LIVE_BYTES,
             (unsigned long)CHURN_TARGET_SLACK_BYTES,
             (unsigned long)CHURN_TARGET_WRITTEN_BYTES);

    while (total_written < CHURN_TARGET_WRITTEN_BYTES) {
        size_class_t cls;
        uint32_t size;
        int replace = 0;
        int slot = -1;

        if (!forced_large_written && total_written >= CHURN_FORCE_LARGE_AFTER_BYTES) {
            size = forced_large_churn_size(&cls);
            forced_large_written = 1;
        } else {
            size = choose_churn_size(&cls);
        }

        while (live_bytes + size > target_live_bytes + CHURN_TARGET_SLACK_BYTES) {
            int del = choose_live_slot();
            if (del < 0 || delete_slot(del, &live_bytes, &delete_ops) != 0) {
                break;
            }
        }

        if (live_bytes > target_live_bytes && (prng_next() & 7u) == 0u) {
            int del = choose_live_slot();
            if (del >= 0) {
                delete_slot(del, &live_bytes, &delete_ops);
            }
        }

        if ((prng_next() % 100u) < 25u) {
            slot = choose_live_slot();
            if (slot >= 0) {
                replace = 1;
            }
        }
        if (slot < 0) {
            slot = find_free_slot();
        }
        if (slot < 0) {
            int del = choose_live_slot();
            if (del >= 0 && delete_slot(del, &live_bytes, &delete_ops) == 0) {
                slot = del;
            }
        }
        if (slot < 0) {
            ESP_LOGE(TAG, "churn no slot available");
            break;
        }

        if (replace) {
            uint32_t old_size = churn_files[slot].size;
            if (bench_delete_file(churn_files[slot].name) != 0) {
                break;
            }
            live_bytes -= old_size;
            replace_ops++;
        } else {
            snprintf(churn_files[slot].name, sizeof(churn_files[slot].name), "w%04d.bin", slot);
            create_ops++;
        }

        int64_t t0 = now_us();
        int wrc = bench_write_file(churn_files[slot].name, size, total_written ^ (uint32_t)slot);
        int64_t elapsed = now_us() - t0;
        if (wrc != 0) {
            ESP_LOGE(TAG, "churn write failed name=%s rc=%d total_written=%lu live_bytes=%lu",
                     churn_files[slot].name, wrc, (unsigned long)total_written,
                     (unsigned long)live_bytes);
            break;
        }

        churn_files[slot].live = 1;
        churn_files[slot].cls = cls;
        churn_files[slot].size = size;
        if (cls == SIZE_LARGE && protected_large_slot < 0) {
            protected_large_slot = slot;
        }
        live_bytes += size;
        total_written += size;
        write_stats[cls].ops++;
        write_stats[cls].files++;
        write_stats[cls].bytes += size;
        write_stats[cls].time_us += elapsed;
        op++;

        ESP_LOGI(TAG, "churn op=%lu name=%s class=%s size=%lu write_us=%lld write_kib_s=%lu total_written=%lu live=%lu",
                 (unsigned long)op, churn_files[slot].name, class_name(cls),
                 (unsigned long)size, (long long)elapsed,
                 (unsigned long)kib_per_s(size, elapsed),
                 (unsigned long)total_written, (unsigned long)live_bytes);

        if ((op % 25u) == 0u || total_written >= CHURN_TARGET_WRITTEN_BYTES) {
            int64_t now = now_us();
            uint32_t interval_bytes = total_written - last_progress_written;
            int64_t interval_us = now - last_progress_us;
            uint32_t live_files = count_live_files();
            ESP_LOGI(TAG, "churn progress ops=%lu written=%lu live=%lu creates=%lu replaces=%lu deletes=%lu",
                     (unsigned long)op, (unsigned long)total_written, (unsigned long)live_bytes,
                     (unsigned long)create_ops, (unsigned long)replace_ops,
                     (unsigned long)delete_ops);
            ESP_LOGI(TAG, "churn progress interval_bytes=%lu interval_us=%lld interval_kib_s=%lu live_files=%lu forced_large=%lu",
                     (unsigned long)interval_bytes, (long long)interval_us,
                     (unsigned long)kib_per_s(interval_bytes, interval_us),
                     (unsigned long)live_files, (unsigned long)forced_large_written);
            log_fs_info("churn progress");
            log_write_health("churn progress");
            last_progress_written = total_written;
            last_progress_us = now;
        }
        vTaskDelay(1);
    }

    ESP_LOGI(TAG, "churn summary ops=%lu written=%lu live=%lu creates=%lu replaces=%lu deletes=%lu",
             (unsigned long)op, (unsigned long)total_written, (unsigned long)live_bytes,
             (unsigned long)create_ops, (unsigned long)replace_ops, (unsigned long)delete_ops);
    log_class_stats("churn write", write_stats);
    log_write_health("churn final");
    log_live_distribution();
    log_fs_info("churn final");
    bench_list();
    run_churn_cold_reads();
}

void run_vfs_benchmarks(void)
{
    int64_t t0;
    int64_t elapsed;
    uint32_t bytes;
    read_stats_t tiny_read_stats = {0};
    read_stats_t med_read_stats = {0};

    ESP_LOGI(TAG, "%s VFS benchmark start", bench_backend_name());
    run_raw_partition_bench();

    t0 = now_us();
    esp_err_t err = bench_backend_mount(false);
    ESP_LOGI(TAG, "initial mount rc=%s time_us=%lld", esp_err_to_name(err),
             (long long)(now_us() - t0));
    if (err == ESP_OK) {
        (void)bench_backend_unmount();
    }

    if (format_and_mount("baseline") != 0) {
        return;
    }
    size_t baseline_used = log_fs_info("baseline overhead_base");

    uint32_t ok_files = 0;
    uint32_t ok_bytes = 0;
    uint32_t errors = 0;

    t0 = now_us();
    for (int i = 0; i < BENCH_TINY_FILES; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "t%04d.bin", i);
        if (bench_write_file(name, BENCH_TINY_SIZE, (uint32_t)i) == 0) {
            ok_files++;
            ok_bytes += BENCH_TINY_SIZE;
        } else {
            errors++;
        }
    }
    elapsed = now_us() - t0;
    ESP_LOGI(TAG, "write tiny files=%lu attempted=%d errors=%lu size=%d bytes=%lu time_us=%lld kib_s=%lu",
             (unsigned long)ok_files, BENCH_TINY_FILES, (unsigned long)errors,
             BENCH_TINY_SIZE, (unsigned long)ok_bytes, (long long)elapsed,
             (unsigned long)kib_per_s(ok_bytes, elapsed));
    size_t after_tiny_used = log_fs_info("after tiny");
    log_storage_overhead("after tiny", ok_files, ok_bytes, baseline_used, after_tiny_used);
    log_write_health("after tiny");

    bench_list();

    t0 = now_us();
    bytes = 0;
    for (int i = 0; i < BENCH_TINY_FILES; ++i) {
        char name[24];
        int idx = permuted_index(i, BENCH_TINY_FILES, 73, 19);
        snprintf(name, sizeof(name), "t%04d.bin", idx);
        bytes += bench_read_file_timed(name, &tiny_read_stats);
    }
    elapsed = now_us() - t0;
    ESP_LOGI(TAG, "read tiny files=%d bytes=%lu time_us=%lld kib_s=%lu",
             BENCH_TINY_FILES, (unsigned long)bytes, (long long)elapsed,
             (unsigned long)kib_per_s(bytes, elapsed));
    log_read_stats("read tiny split", &tiny_read_stats);

    ok_files = 0;
    ok_bytes = 0;
    errors = 0;

    t0 = now_us();
    for (int i = 0; i < BENCH_MED_FILES; ++i) {
        char name[24];
        snprintf(name, sizeof(name), "m%04d.bin", i);
        if (bench_write_file(name, BENCH_MED_SIZE, 0x10000u + (uint32_t)i) == 0) {
            ok_files++;
            ok_bytes += BENCH_MED_SIZE;
        } else {
            errors++;
        }
    }
    elapsed = now_us() - t0;
    ESP_LOGI(TAG, "write medium files=%lu attempted=%d errors=%lu size=%d bytes=%lu time_us=%lld kib_s=%lu",
             (unsigned long)ok_files, BENCH_MED_FILES, (unsigned long)errors,
             BENCH_MED_SIZE, (unsigned long)ok_bytes, (long long)elapsed,
             (unsigned long)kib_per_s(ok_bytes, elapsed));
    size_t after_medium_used = log_fs_info("after medium");
    log_storage_overhead("after medium", BENCH_TINY_FILES + ok_files,
                         BENCH_TINY_FILES * BENCH_TINY_SIZE + ok_bytes,
                         baseline_used, after_medium_used);
    log_write_health("after medium");

    t0 = now_us();
    bytes = 0;
    for (int i = 0; i < BENCH_MED_FILES; ++i) {
        char name[24];
        int idx = permuted_index(i, BENCH_MED_FILES, 5, 3);
        snprintf(name, sizeof(name), "m%04d.bin", idx);
        bytes += bench_read_file_timed(name, &med_read_stats);
    }
    elapsed = now_us() - t0;
    ESP_LOGI(TAG, "read medium files=%d bytes=%lu time_us=%lld kib_s=%lu",
             BENCH_MED_FILES, (unsigned long)bytes, (long long)elapsed,
             (unsigned long)kib_per_s(bytes, elapsed));
    log_read_stats("read medium split", &med_read_stats);

    bench_list();
    bench_tiny_position_stats();
    bench_exists_baseline();
    bench_cold_start_phase();
    run_churn_workload();
}
