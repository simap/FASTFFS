#include "fastffs/fastffs.h"
#include "fastffs/fastffs_host.h"
#include "fastffs/fastffs_inspect.h"
#include "fastffs/verify_flash.h"
#include "churn_model.h"
#include "../src/fffs_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
#define API_SWEEP_INDEX_HEADS FFFS_SLOT_COUNT
#else
#define API_SWEEP_INDEX_HEADS FFFS_INDEX_HASH_TABLE_SIZE
#endif

enum {
    API_SWEEP_SECTOR_SIZE = 256,
    API_SWEEP_SECTOR_COUNT = 256,
    API_SWEEP_INDEX_SECTORS = 3,
    API_SWEEP_FILE_COUNT = BENCH_CHURN_MAX_FILES,
    API_SWEEP_DEFAULT_MAX_STEPS = 262144,
    API_SWEEP_MAX_CONTENT = (API_SWEEP_SECTOR_SIZE * API_SWEEP_SECTOR_COUNT *
            40u) / 100u,
    API_SWEEP_PROGRAM_GRANULE = 4,
    API_SWEEP_SCRATCH_SIZE = 4096,
    API_SWEEP_MAX_CRASH_POINTS = 262144,
    API_SWEEP_CHURN_GC_STEPS = 8,
    API_SWEEP_PROGRESS_INTERVAL_SEC = 10,
    API_SWEEP_DEFAULT_THREADS = 8,
    API_SWEEP_MAX_THREADS = 64,
};

enum api_step_type {
    API_STEP_OPEN_WRITE = 0,
    API_STEP_WRITE = 1,
    API_STEP_CLOSE = 2,
    API_STEP_DELETE = 3,
    API_STEP_GC = 4,
};

struct api_step {
    enum api_step_type type;
    uint16_t file_id;
    uint16_t tx_id;
    uint16_t offset;
    uint16_t size;
    char name[FFFS_MAX_NAME + 1];
};

struct model_file {
    bool exists;
    uint16_t file_id;
    uint16_t tx_id;
    size_t size;
    char name[FFFS_MAX_NAME + 1];
};

struct api_model {
    struct model_file files[API_SWEEP_FILE_COUNT];
};

struct open_tx {
    bool active;
    uint16_t file_id;
    uint16_t tx_id;
    size_t size;
    size_t written;
    char name[FFFS_MAX_NAME + 1];
    uint8_t *data;
};

struct crash_point {
    uint64_t sequence;
    uint64_t allowed_a;
    uint64_t allowed_b;
    uint32_t workload_id;
    uint16_t step_index;
    uint8_t step_type;
};

struct api_stats {
    uint64_t crash_points;
    uint64_t random_workloads;
    uint64_t invariant_failures;
    uint64_t churn_writes[BENCH_CHURN_CLASS_COUNT];
    uint64_t churn_deletes;
    uint64_t generated_write_bytes;
    uint64_t generated_steps;
    uint64_t truncated_workloads;
};

struct wear_stats {
    bool valid;
    uint32_t seed;
    size_t sectors;
    size_t erased;
    uint32_t min;
    uint32_t max;
    uint64_t total;
};

struct wear_summary {
    size_t samples;
    size_t sectors;
    size_t erased;
    uint32_t min;
    uint32_t max;
    uint64_t total;
};

struct worker_status {
    bool active;
    bool done;
    uint32_t seed;
    size_t steps;
    size_t crash_index;
    size_t crash_count;
    uint64_t crash_points;
    uint64_t failures;
    int err;
};

struct dispatcher {
    pthread_mutex_t mutex;
    uint32_t seed_start;
    size_t seed_count;
    size_t next_seed_index;
    size_t tx_per_seed;
    size_t target_write_multiple;
    size_t max_steps;
    size_t thread_count;
    uint64_t next_report_ns;
    int err;
    struct worker_status *statuses;
};

struct worker_ctx {
    size_t thread_id;
    char log_path[256];
    FILE *log;
    struct dispatcher *dispatcher;
    struct api_stats stats;
    struct wear_stats last_wear;
};

static const char *step_type_name(enum api_step_type type) {
    switch (type) {
    case API_STEP_OPEN_WRITE:
        return "open_write";
    case API_STEP_WRITE:
        return "write";
    case API_STEP_CLOSE:
        return "close";
    case API_STEP_DELETE:
        return "delete";
    case API_STEP_GC:
        return "gc";
    default:
        return "unknown";
    }
}

struct mount_storage {
    uint16_t *index_heads;
    uint8_t scratch[API_SWEEP_SCRATCH_SIZE];
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    uint32_t alloc_map[512];
#endif
};

static void format_count(char *out, size_t out_size, uint64_t value);

static const char *cache_mode_name(void) {
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_NONE
    return "none";
#elif FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS
    return "hash-heads";
#elif FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
    return "full-slot-heads";
#else
    return "unknown";
#endif
}

static uint64_t fnv1a_update(uint64_t hash, const void *data, size_t size) {
    const uint8_t *p = data;
    for (size_t i = 0; i < size; i++) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t fnv1a_u64(uint64_t hash, uint64_t value) {
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof(bytes); i++) {
        bytes[i] = (uint8_t)(value >> (i * 8u));
    }
    return fnv1a_update(hash, bytes, sizeof(bytes));
}

static void make_content(uint16_t tx_id, uint16_t file_id,
        uint8_t *data, size_t size) {
    uint32_t x = UINT32_C(0x9e3779b9) ^ ((uint32_t)tx_id << 8) ^ file_id;
    for (size_t i = 0; i < size; i++) {
        x = x * UINT32_C(1664525) + UINT32_C(1013904223);
        data[i] = (uint8_t)(x >> 24) ^ (uint8_t)i;
    }
}

static uint64_t hash_generated_content(uint64_t hash, uint16_t tx_id,
        uint16_t file_id, size_t size) {
    uint8_t buf[128];
    size_t off = 0;
    uint32_t x = UINT32_C(0x9e3779b9) ^ ((uint32_t)tx_id << 8) ^
        file_id;
    while (off < size) {
        size_t n = size - off < sizeof(buf) ? size - off : sizeof(buf);
        for (size_t i = 0; i < n; i++) {
            x = x * UINT32_C(1664525) + UINT32_C(1013904223);
            buf[i] = (uint8_t)(x >> 24) ^ (uint8_t)(off + i);
        }
        hash = fnv1a_update(hash, buf, n);
        off += n;
    }
    return hash;
}

static int new_flash(struct ffsv_flash **flash, struct fffs_backend *backend) {
    struct ffsv_flash_config cfg;
    int err = ffsv_flash_config_preset(&cfg, FFSV_PRESET_TARGET_NOR_NOTES,
            API_SWEEP_SECTOR_SIZE * API_SWEEP_SECTOR_COUNT);
    if (err != FFSV_OK) {
        return FFFS_ERR_IO;
    }
    cfg.sector_size = API_SWEEP_SECTOR_SIZE;
    cfg.max_log_entries = 300000;
    err = ffsv_flash_create(flash, &cfg);
    if (err != FFSV_OK) {
        return FFFS_ERR_IO;
    }
    return fffs_host_backend_from_verify_flash(backend, *flash);
}

static int mount_storage_init(struct mount_storage *storage) {
    memset(storage, 0, sizeof(*storage));
    storage->index_heads = calloc(API_SWEEP_INDEX_HEADS,
            sizeof(*storage->index_heads));
    return storage->index_heads ? FFFS_OK : FFFS_ERR_NOMEM;
}

static void mount_storage_destroy(struct mount_storage *storage) {
    free(storage->index_heads);
    *storage = (struct mount_storage){0};
}

static int mount_fs(struct fffs *fs, const struct fffs_backend *backend,
        struct mount_storage *storage) {
    memset(storage->index_heads, 0,
            API_SWEEP_INDEX_HEADS * sizeof(*storage->index_heads));
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    memset(storage->alloc_map, 0, sizeof(storage->alloc_map));
#endif
    return fffs_mount(fs, backend, &(struct fffs_mount_options){
        .index_heads = storage->index_heads,
        .index_hash_table_size = API_SWEEP_INDEX_HEADS,
        .scratch = storage->scratch,
        .scratch_size = sizeof(storage->scratch),
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
        .alloc_map = storage->alloc_map,
        .alloc_map_words = sizeof(storage->alloc_map) /
            sizeof(storage->alloc_map[0]),
#endif
    });
}

static int model_file_name_cmp(const void *a, const void *b) {
    const struct model_file * const *fa = a;
    const struct model_file * const *fb = b;
    return strcmp((*fa)->name, (*fb)->name);
}

static uint64_t model_hash(const struct api_model *model) {
    const struct model_file *entries[API_SWEEP_FILE_COUNT];
    size_t count = 0;
    for (size_t i = 0; i < API_SWEEP_FILE_COUNT; i++) {
        if (model->files[i].exists) {
            entries[count++] = &model->files[i];
        }
    }
    qsort(entries, count, sizeof(entries[0]), model_file_name_cmp);

    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < count; i++) {
        const struct model_file *file = entries[i];
        hash = fnv1a_update(hash, file->name, strlen(file->name) + 1);
        hash = fnv1a_u64(hash, file->size);
        hash = hash_generated_content(hash, file->tx_id, file->file_id,
                file->size);
    }
    return hash;
}

static int stat_name_cmp(const void *a, const void *b) {
    const struct fffs_stat *sa = a;
    const struct fffs_stat *sb = b;
    return strcmp(sa->name, sb->name);
}

static int namespace_hash(struct fffs *fs, uint64_t *out_hash) {
    struct fffs_stat entries[API_SWEEP_FILE_COUNT + 8];
    size_t count = 0;
    int err = fffs_list(fs, entries, sizeof(entries) / sizeof(entries[0]),
            &count);
    if (err != FFFS_OK) {
        return err;
    }
    if (count > sizeof(entries) / sizeof(entries[0])) {
        return FFFS_ERR_CORRUPT;
    }
    qsort(entries, count, sizeof(entries[0]), stat_name_cmp);

    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < count; i++) {
        struct fffs_file file;
        uint8_t buf[97];
        size_t remaining = entries[i].size;
        hash = fnv1a_update(hash, entries[i].name,
                strlen(entries[i].name) + 1);
        hash = fnv1a_u64(hash, entries[i].size);
        err = fffs_open(fs, &file, entries[i].name, FFFS_O_RDONLY);
        if (err != FFFS_OK) {
            return err;
        }
        while (remaining > 0) {
            size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
            size_t nread = 0;
            err = fffs_read(&file, buf, want, &nread);
            if (err != FFFS_OK || nread == 0 || nread > remaining) {
                (void)fffs_close(&file);
                return err != FFFS_OK ? err : FFFS_ERR_CORRUPT;
            }
            hash = fnv1a_update(hash, buf, nread);
            remaining -= nread;
        }
        err = fffs_close(&file);
        if (err != FFFS_OK) {
            return err;
        }
    }
    *out_hash = hash;
    return FFFS_OK;
}

static int check_image(struct fffs_backend *backend, struct fffs *fs,
        uint64_t allowed_a, uint64_t allowed_b) {
    struct fffs_inspect_summary summary;
    int err = fffs_inspect_check(backend, &summary);
    if (err != FFFS_OK) {
        return err;
    }
    if (summary.index_corrupt_records || summary.live_entries_corrupt ||
            summary.data_sectors_corrupt || summary.md_corrupt) {
        return FFFS_ERR_CORRUPT;
    }
    uint64_t actual;
    err = namespace_hash(fs, &actual);
    if (err != FFFS_OK) {
        return err;
    }
    return actual == allowed_a || actual == allowed_b ? FFFS_OK :
        FFFS_ERR_CORRUPT;
}

static int apply_step(struct fffs *fs, const struct api_step *step,
        struct fffs_file *open_file, struct open_tx *tx) {
    switch (step->type) {
    case API_STEP_OPEN_WRITE:
        if (tx->active) {
            return FFFS_ERR_INVALID;
        }
        tx->active = true;
        tx->file_id = step->file_id;
        tx->tx_id = step->tx_id;
        tx->size = step->size;
        tx->written = 0;
        snprintf(tx->name, sizeof(tx->name), "%s", step->name);
        make_content(step->tx_id, step->file_id, tx->data, tx->size);
        return fffs_open(fs, open_file, step->name,
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC);
    case API_STEP_WRITE: {
        if (!tx->active || step->offset > tx->size ||
                step->size > tx->size - step->offset) {
            return FFFS_ERR_INVALID;
        }
        size_t written = 0;
        int err = fffs_write(open_file, tx->data + step->offset,
                step->size, &written);
        if (err == FFFS_OK && written != step->size) {
            return FFFS_ERR_IO;
        }
        if (err == FFFS_OK) {
            tx->written += written;
        }
        return err;
    }
    case API_STEP_CLOSE:
        if (!tx->active || tx->written != tx->size) {
            return FFFS_ERR_INVALID;
        }
        tx->active = false;
        return fffs_close(open_file);
    case API_STEP_DELETE:
        if (tx->active) {
            return FFFS_ERR_INVALID;
        }
        {
            int err = fffs_delete_file(fs, step->name);
            return err == FFFS_ERR_NOT_FOUND ? FFFS_OK : err;
        }
    case API_STEP_GC: {
        size_t erased = 0;
        return fffs_gc(fs, step->size, &erased);
    }
    default:
        return FFFS_ERR_INVALID;
    }
}

static void model_apply_step(struct api_model *model,
        const struct api_step *step, const struct open_tx *before_tx) {
    if (step->type == API_STEP_CLOSE && before_tx->active) {
        struct model_file *f = &model->files[before_tx->file_id];
        f->exists = true;
        f->file_id = before_tx->file_id;
        f->tx_id = before_tx->tx_id;
        f->size = before_tx->size;
        snprintf(f->name, sizeof(f->name), "%s", before_tx->name);
    } else if (step->type == API_STEP_DELETE) {
        model->files[step->file_id].exists = false;
    }
}

static int collect_crash_points(const struct ffsv_op_record *log,
        size_t first, size_t count, struct crash_point *points,
        size_t *point_count, uint64_t allowed_a, uint64_t allowed_b,
        uint32_t workload_id, uint16_t step_index, uint8_t step_type) {
    for (size_t i = first; i < count; i++) {
        if (log[i].type != FFSV_OP_PROGRAM && log[i].type != FFSV_OP_ERASE) {
            continue;
        }
        if (*point_count >= API_SWEEP_MAX_CRASH_POINTS) {
            return FFFS_ERR_RANGE;
        }
        points[(*point_count)++] = (struct crash_point){
            .sequence = log[i].sequence,
            .allowed_a = allowed_a,
            .allowed_b = allowed_b,
            .workload_id = workload_id,
            .step_index = step_index,
            .step_type = step_type,
        };
    }
    return FFFS_OK;
}

static bool log_has_injected_sequence(const struct ffsv_flash *flash,
        uint64_t sequence) {
    size_t count = 0;
    const struct ffsv_op_record *log = ffsv_flash_log(flash, &count);
    for (size_t i = 0; i < count; i++) {
        if (log[i].sequence == sequence && log[i].injected) {
            return true;
        }
    }
    return false;
}

static int run_steps(struct ffsv_flash *flash, struct fffs_backend *backend,
        const struct api_step *steps, size_t step_count,
        struct crash_point *points, size_t *point_count,
        const struct crash_point *target, bool *hit_target,
        uint32_t workload_id) {
    struct fffs fs;
    struct fffs_file open_file;
    struct mount_storage storage;
    struct api_model model = {0};
    struct open_tx tx = {0};
    tx.data = malloc(API_SWEEP_MAX_CONTENT);
    if (!tx.data) {
        return FFFS_ERR_NOMEM;
    }

    int err = mount_storage_init(&storage);
    if (err != FFFS_OK) {
        free(tx.data);
        return err;
    }
    err = mount_fs(&fs, backend, &storage);
    if (err != FFFS_OK) {
        mount_storage_destroy(&storage);
        free(tx.data);
        return err;
    }

    if (target) {
        ffsv_flash_set_failure(flash, &(struct ffsv_failure_injection){
            .enabled = true,
            .sequence = target->sequence,
            .op_mask = (UINT32_C(1) << FFSV_OP_PROGRAM) |
                (UINT32_C(1) << FFSV_OP_ERASE),
            .phase = FFSV_FAIL_AFTER,
            .status = FFSV_ERR_INJECTED,
        });
    }

    for (size_t i = 0; i < step_count; i++) {
        struct api_model before_model = model;
        struct api_model after_model = model;
        struct open_tx before_tx = tx;
        model_apply_step(&after_model, &steps[i], &before_tx);
        uint64_t before_hash = model_hash(&before_model);
        uint64_t after_hash = model_hash(&after_model);
        size_t log_before = 0;
        (void)ffsv_flash_log(flash, &log_before);

        err = apply_step(&fs, &steps[i], &open_file, &tx);
        if (!target && err != FFFS_OK) {
            break;
        }
        if (!target) {
            size_t log_count = 0;
            const struct ffsv_op_record *log = ffsv_flash_log(flash,
                    &log_count);
            err = collect_crash_points(log, log_before, log_count, points,
                    point_count, before_hash, after_hash, workload_id,
                    (uint16_t)i, (uint8_t)steps[i].type);
            if (err != FFFS_OK) {
                break;
            }
        }
        if (target && log_has_injected_sequence(flash, target->sequence)) {
            *hit_target = true;
            err = FFFS_OK;
            break;
        }
        if (target && err != FFFS_OK) {
            err = FFFS_OK;
            break;
        }
        if (err == FFFS_OK) {
            model = after_model;
        }
    }

    ffsv_flash_clear_failure(flash);
    fffs_unmount(&fs);
    mount_storage_destroy(&storage);
    free(tx.data);
    return err;
}

static int init_formatted_flash(struct ffsv_flash **flash,
        struct fffs_backend *backend) {
    int err = new_flash(flash, backend);
    if (err != FFFS_OK) {
        return err;
    }
    return fffs_format(backend, &(struct fffs_format_options){
        .index_sectors = API_SWEEP_INDEX_SECTORS,
        .sector_size = FFFS_SECTOR_256,
    });
}

static void log_flash_wear(FILE *log, const struct ffsv_flash *flash,
        uint32_t workload_id, struct wear_stats *wear_stats) {
    const struct ffsv_flash_config *cfg = ffsv_flash_config(flash);
    size_t sector_count = cfg->total_size / cfg->sector_size;
    uint64_t total = 0;
    uint32_t min = UINT32_MAX;
    uint32_t max = 0;
    size_t erased = 0;
    for (size_t i = 0; i < sector_count; i++) {
        uint32_t wear = ffsv_flash_sector_wear(flash, i);
        if (wear < min) {
            min = wear;
        }
        if (wear > max) {
            max = wear;
        }
        if (wear > 0) {
            erased++;
        }
        total += wear;
    }
    if (wear_stats) {
        *wear_stats = (struct wear_stats){
            .valid = true,
            .seed = workload_id,
            .sectors = sector_count,
            .erased = erased,
            .min = min,
            .max = max,
            .total = total,
        };
    }
    if (log) {
        uint64_t avg_x100 = (total * 100u) / sector_count;
        fprintf(log,
                "seed=0x%08x wear_summary sectors=%zu erased=%zu min=%u "
                "max=%u avg=%llu.%02llu\n",
                (unsigned)workload_id, sector_count, erased, (unsigned)min,
                (unsigned)max,
                (unsigned long long)(avg_x100 / 100u),
                (unsigned long long)(avg_x100 % 100u));
        fprintf(log, "seed=0x%08x wear", (unsigned)workload_id);
        for (size_t i = 0; i < sector_count; i++) {
            fprintf(log, " %u", (unsigned)ffsv_flash_sector_wear(flash, i));
        }
        fprintf(log, "\n");
        fflush(log);
    }
}

static int collect_workload_points(const struct api_step *steps,
        size_t step_count, struct crash_point *points, size_t *point_count,
        uint32_t workload_id, FILE *log, struct wear_stats *wear_stats) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    bool hit = false;
    int err = init_formatted_flash(&flash, &backend);
    if (err == FFFS_OK) {
        err = run_steps(flash, &backend, steps, step_count, points,
                point_count, NULL, &hit, workload_id);
        if (err == FFFS_OK) {
            log_flash_wear(log, flash, workload_id, wear_stats);
        }
    }
    ffsv_flash_destroy(flash);
    return err;
}

static int replay_crash_point(const struct api_step *steps, size_t step_count,
        const struct crash_point *point) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    bool hit = false;
    int err = init_formatted_flash(&flash, &backend);
    if (err == FFFS_OK) {
        err = run_steps(flash, &backend, steps, step_count, NULL, NULL,
                point, &hit, point->workload_id);
    }
    if (err == FFFS_OK && !hit) {
        err = FFFS_ERR_CORRUPT;
    }
    if (err == FFFS_OK) {
        struct fffs recovered;
        struct mount_storage storage;
        err = mount_storage_init(&storage);
        if (err == FFFS_OK) {
            err = mount_fs(&recovered, &backend, &storage);
        }
        if (err == FFFS_OK) {
            err = check_image(&backend, &recovered, point->allowed_a,
                    point->allowed_b);
            fffs_unmount(&recovered);
        }
        mount_storage_destroy(&storage);
    }
    ffsv_flash_destroy(flash);
    return err;
}

static uint32_t rng_next(uint32_t *rng) {
    *rng = *rng * UINT32_C(1664525) + UINT32_C(1013904223);
    return *rng;
}

static uint64_t progress_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
        (uint64_t)ts.tv_nsec;
}

static void append_worker_status(char *out, size_t out_size, size_t id,
        const struct worker_status *status) {
    char remaining[32];
    char failures[32];
    size_t crash_remaining = status->crash_count > status->crash_index ?
        status->crash_count - status->crash_index : 0;
    snprintf(remaining, sizeof(remaining), "%zu", crash_remaining);
    snprintf(failures, sizeof(failures), "%llu",
            (unsigned long long)status->failures);
    size_t used = strlen(out);
    if (used >= out_size) {
        return;
    }
    const char *sep = used == 0 ? "" : " ";
    if (status->active) {
        (void)id;
        snprintf(out + used, out_size - used, "%s%08x,%s,%s",
                sep, (unsigned)status->seed, remaining, failures);
    } else {
        (void)id;
        snprintf(out + used, out_size - used, "%s%s,%s,%s",
                sep, status->done ? "done" : "idle", remaining, failures);
    }
}

static void print_all_worker_status_locked(const struct dispatcher *dispatcher) {
    char line[2048];
    line[0] = '\0';
    for (size_t i = 0; i < dispatcher->thread_count; i++) {
        append_worker_status(line, sizeof(line), i, &dispatcher->statuses[i]);
    }
    printf("%s\n", line);
    fflush(stdout);
}

static void maybe_report_worker_status(struct dispatcher *dispatcher) {
    uint64_t now = progress_now_ns();
    if (now == 0) {
        return;
    }
    pthread_mutex_lock(&dispatcher->mutex);
    if (now >= dispatcher->next_report_ns) {
        dispatcher->next_report_ns = now +
            (uint64_t)API_SWEEP_PROGRESS_INTERVAL_SEC *
            UINT64_C(1000000000);
        print_all_worker_status_locked(dispatcher);
    }
    pthread_mutex_unlock(&dispatcher->mutex);
}

static void update_worker_status(struct dispatcher *dispatcher,
        size_t thread_id, const struct worker_status *update) {
    pthread_mutex_lock(&dispatcher->mutex);
    dispatcher->statuses[thread_id] = *update;
    pthread_mutex_unlock(&dispatcher->mutex);
}

static size_t append_gc_step(struct api_step *steps, size_t pos,
        size_t capacity) {
    if (pos < capacity) {
        steps[pos++] = (struct api_step){
            .type = API_STEP_GC,
            .size = API_SWEEP_CHURN_GC_STEPS,
        };
    }
    return pos;
}

static size_t append_write_tx(struct api_step *steps, size_t pos,
        size_t capacity, uint16_t file_id, const char *name,
        uint16_t tx_id, size_t size, uint32_t *rng, bool *complete) {
    size_t start = pos;
    *complete = false;
    if (pos >= capacity) {
        return pos;
    }
    steps[pos++] = (struct api_step){
        .type = API_STEP_OPEN_WRITE,
        .file_id = file_id,
        .tx_id = tx_id,
        .size = (uint16_t)size,
    };
    snprintf(steps[pos - 1].name, sizeof(steps[pos - 1].name), "%s", name);
    size_t off = 0;
    while (off < size && pos < capacity) {
        uint32_t r = rng_next(rng);
        size_t chunk = 17u + (r % 149u);
        if (chunk > size - off) {
            chunk = size - off;
        }
        steps[pos++] = (struct api_step){
            .type = API_STEP_WRITE,
            .file_id = file_id,
            .tx_id = tx_id,
            .offset = (uint16_t)off,
            .size = (uint16_t)chunk,
        };
        snprintf(steps[pos - 1].name, sizeof(steps[pos - 1].name), "%s",
                name);
        off += chunk;
        if ((r & 1u) == 0u) {
            pos = append_gc_step(steps, pos, capacity);
        }
    }
    if (pos < capacity) {
        steps[pos++] = (struct api_step){
            .type = API_STEP_CLOSE,
            .file_id = file_id,
            .tx_id = tx_id,
        };
        snprintf(steps[pos - 1].name, sizeof(steps[pos - 1].name), "%s",
                name);
        *complete = true;
    } else {
        return start;
    }
    pos = append_gc_step(steps, pos, capacity);
    return pos;
}

static size_t scaled_churn_size(const bench_churn_event_t *event) {
    enum {
        large_min = API_SWEEP_MAX_CONTENT * 3u / 4u,
        large_span = API_SWEEP_MAX_CONTENT - large_min + 1u,
    };
    switch (event->cls) {
    case BENCH_CHURN_CLASS_SMALL:
        return 16u + (event->write_seed % 145u);
    case BENCH_CHURN_CLASS_MEDIUM:
        return 600u + (event->write_seed % 1800u);
    case BENCH_CHURN_CLASS_LARGE:
        return large_min + (event->write_seed % large_span);
    default:
        return 96u;
    }
}

static size_t api_sweep_file_data_per_sector(void) {
    size_t raw = API_SWEEP_SECTOR_SIZE - FFFS_SECTOR_FOOTER_SIZE -
        FFFS_MD_SIZE;
    return raw - (raw % API_SWEEP_PROGRAM_GRANULE);
}

static size_t api_sweep_file_sector_count(size_t payload_size) {
    size_t data_per_sector = api_sweep_file_data_per_sector();
    if (payload_size == 0) {
        return 1;
    }
    return (payload_size + data_per_sector - 1u) / data_per_sector;
}

static size_t estimated_file_footprint(size_t payload_size) {
    return api_sweep_file_sector_count(payload_size) * API_SWEEP_SECTOR_SIZE;
}

static size_t churn_workload(uint32_t seed, struct api_step *steps,
        size_t capacity, size_t tx_count, size_t target_write_multiple,
        struct api_stats *stats) {
    bench_churn_model_t model;
    uint32_t rng = seed ^ UINT32_C(0x5eed1234);
    size_t pos = 0;
    size_t tx = 0;
    uint64_t actual_written = 0;
    uint64_t image_size = (uint64_t)API_SWEEP_SECTOR_SIZE *
        API_SWEEP_SECTOR_COUNT;
    uint64_t target_written64 = image_size * target_write_multiple;
    uint32_t force_large_after = target_written64 / 10u > UINT32_MAX ?
        UINT32_MAX : (uint32_t)(target_written64 / 10u);
    if (force_large_after < 20u * 1024u) {
        force_large_after = 20u * 1024u;
    }

    bench_churn_model_init(&model, seed,
            API_SWEEP_SECTOR_SIZE * API_SWEEP_SECTOR_COUNT * 75u / 100u,
            UINT32_MAX, API_SWEEP_SECTOR_SIZE * API_SWEEP_SECTOR_COUNT / 8u,
            force_large_after);

    while (tx < tx_count && pos < capacity && actual_written < target_written64) {
        bench_churn_event_t event;
        bench_churn_event_type_t type = bench_churn_model_next(&model,
                &event);
        if (type == BENCH_CHURN_EVENT_DONE ||
                type == BENCH_CHURN_EVENT_NO_SLOT) {
            break;
        }
        if (type == BENCH_CHURN_EVENT_DELETE) {
            if (pos < capacity) {
                steps[pos++] = (struct api_step){
                    .type = API_STEP_DELETE,
                    .file_id = (uint16_t)event.slot,
                };
                snprintf(steps[pos - 1].name, sizeof(steps[pos - 1].name),
                        "%s", event.name);
            }
            pos = append_gc_step(steps, pos, capacity);
            bench_churn_model_apply(&model, &event);
            stats->churn_deletes++;
            tx++;
            continue;
        }
        size_t size = scaled_churn_size(&event);
        if (size > API_SWEEP_MAX_CONTENT) {
            size = API_SWEEP_MAX_CONTENT;
        }
        size_t model_size = estimated_file_footprint(size);
        event.size = model_size > UINT32_MAX ? UINT32_MAX :
            (uint32_t)model_size;
        bool complete = false;
        pos = append_write_tx(steps, pos, capacity, (uint16_t)event.slot,
                event.name, (uint16_t)(event.write_seed ^ seed), size, &rng,
                &complete);
        if (!complete) {
            break;
        }
        bench_churn_model_apply(&model, &event);
        stats->churn_writes[event.cls]++;
        stats->generated_write_bytes += size;
        actual_written += size;
        tx++;
    }
    return pos;
}

static void log_steps(FILE *log, uint32_t seed, const struct api_step *steps,
        size_t count) {
    for (size_t i = 0; i < count; i++) {
        fprintf(log,
                "seed=0x%08x step=%zu type=%s file=%u name=%s tx=%u "
                "off=%u size=%u\n",
                (unsigned)seed, i, step_type_name(steps[i].type),
                (unsigned)steps[i].file_id, steps[i].name,
                (unsigned)steps[i].tx_id, (unsigned)steps[i].offset,
                (unsigned)steps[i].size);
    }
    fflush(log);
}

static int run_workload(const struct api_step *steps, size_t step_count,
        struct api_stats *stats, uint32_t workload_id, FILE *log,
        struct dispatcher *dispatcher, size_t thread_id,
        struct wear_stats *wear_stats) {
    struct crash_point *points = calloc(API_SWEEP_MAX_CRASH_POINTS,
            sizeof(*points));
    if (!points) {
        return FFFS_ERR_NOMEM;
    }
    size_t point_count = 0;
    int err = collect_workload_points(steps, step_count, points,
            &point_count, workload_id, log, wear_stats);
    if (log) {
        fprintf(log, "seed=0x%08x crash_points=%zu\n",
                (unsigned)workload_id, point_count);
        fflush(log);
    }
    for (size_t i = 0; err == FFFS_OK && i < point_count; i++) {
        err = replay_crash_point(steps, step_count, &points[i]);
        stats->crash_points++;
        if (dispatcher) {
            struct worker_status status = {
                .active = true,
                .seed = workload_id,
                .steps = step_count,
                .crash_index = i + 1,
                .crash_count = point_count,
                .crash_points = stats->crash_points,
                .failures = stats->invariant_failures,
                .err = err,
            };
            update_worker_status(dispatcher, thread_id, &status);
            maybe_report_worker_status(dispatcher);
        }
        if (err != FFFS_OK) {
            stats->invariant_failures++;
            fprintf(log,
                    "api crash failure workload=%u step=%u type=%u "
                    "seq=%llu err=%s\n",
                    points[i].workload_id, (unsigned)points[i].step_index,
                    (unsigned)points[i].step_type,
                    (unsigned long long)points[i].sequence,
                    fffs_status_name(err));
            fflush(log);
        }
    }
    free(points);
    return err;
}

static void stats_add(struct api_stats *dst, const struct api_stats *src) {
    dst->crash_points += src->crash_points;
    dst->random_workloads += src->random_workloads;
    dst->invariant_failures += src->invariant_failures;
    dst->churn_deletes += src->churn_deletes;
    dst->generated_write_bytes += src->generated_write_bytes;
    dst->generated_steps += src->generated_steps;
    dst->truncated_workloads += src->truncated_workloads;
    for (size_t i = 0; i < BENCH_CHURN_CLASS_COUNT; i++) {
        dst->churn_writes[i] += src->churn_writes[i];
    }
}

static void wear_summary_add(struct wear_summary *summary,
        const struct wear_stats *stats) {
    if (!stats->valid) {
        return;
    }
    if (summary->samples == 0 || stats->min < summary->min) {
        summary->min = stats->min;
    }
    if (stats->max > summary->max) {
        summary->max = stats->max;
    }
    summary->samples++;
    summary->sectors += stats->sectors;
    summary->erased += stats->erased;
    summary->total += stats->total;
}

static bool dispatcher_next_seed(struct dispatcher *dispatcher,
        size_t *seed_index, uint32_t *seed) {
    bool found = false;
    pthread_mutex_lock(&dispatcher->mutex);
    if (dispatcher->err == FFFS_OK &&
            dispatcher->next_seed_index < dispatcher->seed_count) {
        *seed_index = dispatcher->next_seed_index++;
        *seed = dispatcher->seed_start + (uint32_t)*seed_index;
        found = true;
    }
    pthread_mutex_unlock(&dispatcher->mutex);
    return found;
}

static void dispatcher_set_error(struct dispatcher *dispatcher, int err) {
    if (err == FFFS_OK) {
        return;
    }
    pthread_mutex_lock(&dispatcher->mutex);
    if (dispatcher->err == FFFS_OK) {
        dispatcher->err = err;
    }
    pthread_mutex_unlock(&dispatcher->mutex);
}

static void *random_worker_main(void *arg) {
    struct worker_ctx *ctx = arg;
    struct dispatcher *dispatcher = ctx->dispatcher;
    struct api_step *steps = calloc(dispatcher->max_steps, sizeof(*steps));
    if (!steps) {
        dispatcher_set_error(dispatcher, FFFS_ERR_NOMEM);
        return NULL;
    }

    size_t seed_index = 0;
    uint32_t seed = 0;
    int last_err = FFFS_OK;
    while (dispatcher_next_seed(dispatcher, &seed_index, &seed)) {
        memset(steps, 0, dispatcher->max_steps * sizeof(*steps));
        size_t count = churn_workload(seed, steps, dispatcher->max_steps,
                dispatcher->tx_per_seed, dispatcher->target_write_multiple,
                &ctx->stats);
        struct worker_status status = {
            .active = true,
            .seed = seed,
            .steps = count,
            .crash_points = ctx->stats.crash_points,
            .failures = ctx->stats.invariant_failures,
            .err = FFFS_OK,
        };
        update_worker_status(dispatcher, ctx->thread_id, &status);
        if (ctx->log) {
            fprintf(ctx->log,
                    "thread=%zu seed_index=%zu seed=0x%08x steps=%zu "
                    "tx_cap=%zu\n",
                    ctx->thread_id, seed_index, (unsigned)seed, count,
                    dispatcher->tx_per_seed);
            log_steps(ctx->log, seed, steps, count);
        }
        ctx->stats.generated_steps += count;
        if (count == dispatcher->max_steps) {
            ctx->stats.truncated_workloads++;
            if (ctx->log) {
                fprintf(ctx->log,
                        "seed=0x%08x truncated at max_steps=%zu\n",
                        (unsigned)seed, dispatcher->max_steps);
                fflush(ctx->log);
            }
        }
        int err = run_workload(steps, count, &ctx->stats, seed, ctx->log,
                dispatcher, ctx->thread_id, &ctx->last_wear);
        last_err = err;
        ctx->stats.random_workloads++;
        status.active = false;
        status.done = false;
        status.crash_points = ctx->stats.crash_points;
        status.failures = ctx->stats.invariant_failures;
        status.err = err;
        update_worker_status(dispatcher, ctx->thread_id, &status);
        if (err != FFFS_OK) {
            dispatcher_set_error(dispatcher, err);
            break;
        }
    }

    free(steps);
    struct worker_status done = {
        .active = false,
        .done = true,
        .crash_points = ctx->stats.crash_points,
        .failures = ctx->stats.invariant_failures,
        .err = last_err,
    };
    update_worker_status(dispatcher, ctx->thread_id, &done);
    return NULL;
}

static void make_thread_log_path(char *out, size_t out_size,
        const char *base_path, size_t thread_id) {
    snprintf(out, out_size, "%s.t%zu.log", base_path, thread_id);
}

static int run_random(uint32_t seed_start, size_t seed_count,
        size_t tx_per_seed, size_t target_write_multiple, size_t max_steps,
        size_t thread_count, const char *log_path, struct api_stats *stats,
        struct wear_summary *wear_summary) {
    if (thread_count == 0 || thread_count > API_SWEEP_MAX_THREADS) {
        return FFFS_ERR_INVALID;
    }
    struct worker_status *statuses = calloc(thread_count, sizeof(*statuses));
    struct worker_ctx *ctxs = calloc(thread_count, sizeof(*ctxs));
    pthread_t *threads = calloc(thread_count, sizeof(*threads));
    if (!statuses || !ctxs || !threads) {
        free(statuses);
        free(ctxs);
        free(threads);
        return FFFS_ERR_NOMEM;
    }

    struct dispatcher dispatcher = {
        .seed_start = seed_start,
        .seed_count = seed_count,
        .tx_per_seed = tx_per_seed,
        .target_write_multiple = target_write_multiple,
        .max_steps = max_steps,
        .thread_count = thread_count,
        .next_report_ns = progress_now_ns() +
            (uint64_t)API_SWEEP_PROGRESS_INTERVAL_SEC *
            UINT64_C(1000000000),
        .err = FFFS_OK,
        .statuses = statuses,
    };
    if (pthread_mutex_init(&dispatcher.mutex, NULL) != 0) {
        free(statuses);
        free(ctxs);
        free(threads);
        return FFFS_ERR_IO;
    }

    size_t started = 0;
    int err = FFFS_OK;
    for (size_t i = 0; i < thread_count; i++) {
        ctxs[i].thread_id = i;
        ctxs[i].dispatcher = &dispatcher;
        make_thread_log_path(ctxs[i].log_path, sizeof(ctxs[i].log_path),
                log_path, i);
        ctxs[i].log = fopen(ctxs[i].log_path, "w");
        if (!ctxs[i].log) {
            err = FFFS_ERR_IO;
            dispatcher_set_error(&dispatcher, err);
            break;
        }
        if (pthread_create(&threads[i], NULL, random_worker_main,
                    &ctxs[i]) != 0) {
            err = FFFS_ERR_IO;
            dispatcher_set_error(&dispatcher, err);
            break;
        }
        started++;
    }

    for (size_t i = 0; i < started; i++) {
        (void)pthread_join(threads[i], NULL);
    }
    pthread_mutex_lock(&dispatcher.mutex);
    print_all_worker_status_locked(&dispatcher);
    if (dispatcher.err != FFFS_OK) {
        err = dispatcher.err;
    }
    pthread_mutex_unlock(&dispatcher.mutex);

    for (size_t i = 0; i < thread_count; i++) {
        stats_add(stats, &ctxs[i].stats);
        wear_summary_add(wear_summary, &ctxs[i].last_wear);
        if (ctxs[i].log) {
            fclose(ctxs[i].log);
        }
    }
    pthread_mutex_destroy(&dispatcher.mutex);
    free(statuses);
    free(ctxs);
    free(threads);
    return err;
}

static size_t parse_arg(const char *text, size_t fallback) {
    if (!text) {
        return fallback;
    }
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 0);
    return end && *end == '\0' ? (size_t)value : fallback;
}

static void format_count(char *out, size_t out_size, uint64_t value) {
    char raw[32];
    snprintf(raw, sizeof(raw), "%llu", (unsigned long long)value);
    size_t len = strlen(raw);
    size_t commas = len > 0 ? (len - 1) / 3 : 0;
    if (len + commas + 1 > out_size) {
        snprintf(out, out_size, "%llu", (unsigned long long)value);
        return;
    }
    size_t first = len % 3;
    if (first == 0) {
        first = 3;
    }
    size_t src = 0;
    size_t dst = 0;
    while (src < len) {
        size_t group = src == 0 ? first : 3;
        if (dst != 0) {
            out[dst++] = ',';
        }
        memcpy(out + dst, raw + src, group);
        dst += group;
        src += group;
    }
    out[dst] = '\0';
}

static void print_count_line(const char *label, uint64_t value) {
    char count[32];
    format_count(count, sizeof(count), value);
    printf("%s: %s\n", label, count);
}

static void print_usage(const char *argv0) {
    printf("Usage:\n");
    printf("  %s <seed_start> <seed_count> <tx_per_seed> "
            "<target_write_multiples> [max_steps] [log_base] [threads]\n",
            argv0);
    printf("\n");
    printf("Arguments:\n");
    printf("  seed_start              first PRNG seed, decimal or 0x-prefixed\n");
    printf("  seed_count              number of random workload seeds to run\n");
    printf("  tx_per_seed             transaction cap for each random seed\n");
    printf("  target_write_multiples  churn target as image-size multiples\n");
    printf("  max_steps               maximum generated API steps per seed\n");
    printf("  log_base                base path for per-thread logs\n");
    printf("  threads                 worker thread count\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help              show this help and exit\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s 0xa11ce000 8 10000 256\n", argv0);
    printf("  %s 0xa11ce000 8 10000 256 262144 "
            "fffs_api_crash_sweep.log 8\n", argv0);
    printf("  %s 0xa11ce000 1 50 32 8192 /tmp/fffs_api_smoke 1\n",
            argv0);
}

int main(int argc, char **argv) {
    if (argc == 1 || (argc == 2 &&
                (strcmp(argv[1], "-h") == 0 ||
                 strcmp(argv[1], "--help") == 0))) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc < 5) {
        print_usage(argv[0]);
        return 2;
    }

    uint32_t seed_start = (uint32_t)parse_arg(argc > 1 ? argv[1] : NULL,
            0xa11ce000u);
    size_t seed_count = parse_arg(argc > 2 ? argv[2] : NULL, 5);
    size_t tx_per_seed = parse_arg(argc > 3 ? argv[3] : NULL, 1000);
    size_t target_write_multiple = parse_arg(argc > 4 ? argv[4] : NULL, 128);
    size_t max_steps = parse_arg(argc > 5 ? argv[5] : NULL,
            API_SWEEP_DEFAULT_MAX_STEPS);
    const char *log_path = argc > 6 ? argv[6] : "fffs_api_crash_sweep.log";
    size_t thread_count = parse_arg(argc > 7 ? argv[7] : NULL,
            API_SWEEP_DEFAULT_THREADS);
    struct api_stats stats = {0};
    struct wear_summary wear_summary = {0};

    printf("fffs api crash sweep [%s]\n", cache_mode_name());
    printf("logs: %s.t<N>.log\n", log_path);
    int err = run_random(seed_start, seed_count, tx_per_seed,
            target_write_multiple, max_steps, thread_count, log_path, &stats,
            &wear_summary);
    printf("seed start: 0x%08x\n", (unsigned)seed_start);
    print_count_line("threads", thread_count);
    print_count_line("tx per seed", tx_per_seed);
    print_count_line("target written image multiples", target_write_multiple);
    print_count_line("max steps per seed", max_steps);
    print_count_line("generated API steps", stats.generated_steps);
    print_count_line("generated write bytes", stats.generated_write_bytes);
    print_count_line("truncated workloads", stats.truncated_workloads);
    print_count_line("crash points tested", stats.crash_points);
    print_count_line("random workloads", stats.random_workloads);
    print_count_line("small writes", stats.churn_writes[BENCH_CHURN_CLASS_SMALL]);
    print_count_line("medium writes", stats.churn_writes[BENCH_CHURN_CLASS_MEDIUM]);
    print_count_line("large writes", stats.churn_writes[BENCH_CHURN_CLASS_LARGE]);
    print_count_line("deletes", stats.churn_deletes);
    if (wear_summary.samples > 0 && wear_summary.sectors > 0) {
        print_count_line("wear samples", wear_summary.samples);
        print_count_line("wear erased sectors", wear_summary.erased);
        print_count_line("wear min", wear_summary.min);
        print_count_line("wear max", wear_summary.max);
        uint64_t wear_avg_x100 =
            wear_summary.total * 100u / wear_summary.sectors;
        printf("wear avg: %llu.%02llu\n",
                (unsigned long long)(wear_avg_x100 / 100u),
                (unsigned long long)(wear_avg_x100 % 100u));
    }
    print_count_line("invariant failures", stats.invariant_failures);
    if (err != FFFS_OK) {
        fprintf(stderr, "api crash sweep failed: %s\n", fffs_status_name(err));
        return 1;
    }
    return 0;
}
