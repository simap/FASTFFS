#include "fastffs/fastffs.h"
#include "fastffs/fastffs_host.h"
#include "fastffs/fastffs_inspect.h"
#include "fastffs/verify_flash.h"
#include "churn_model.h"
#include "../src/fffs_internal.h"

#include <errno.h>
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
#define API_SWEEP_INDEX_CACHE_SIZE FFFS_INDEX_CACHE_BYTES(API_SWEEP_INDEX_HEADS)

enum {
    API_SWEEP_DEFAULT_SECTOR_SIZE = 256,
    API_SWEEP_DEFAULT_SECTOR_COUNT = 256,
    API_SWEEP_DEFAULT_INDEX_SECTORS = 3,
    API_SWEEP_FILE_COUNT = BENCH_CHURN_MAX_FILES,
    API_SWEEP_DEFAULT_MAX_STEPS = 262144,
    API_SWEEP_DEFAULT_SEED = UINT32_C(0x46464653),
    API_SWEEP_DEFAULT_RUNS = 1,
    API_SWEEP_DEFAULT_WRITE_MULTIPLE = 2,
    API_SWEEP_DEFAULT_PARTIAL_WRITE_SAMPLES = 1,
    API_SWEEP_DEFAULT_PARTIAL_WRITE_BITS = 2,
    API_SWEEP_PROGRAM_GRANULE = 4,
    API_SWEEP_SCRATCH_SIZE = 4096,
    API_SWEEP_ALLOC_MAP_WORDS = 512,
    API_SWEEP_MAX_CRASH_POINTS = 262144,
    API_SWEEP_CHURN_GC_STEPS = 8,
    API_SWEEP_PROGRESS_INTERVAL_SEC = 10,
    API_SWEEP_PROGRESS_STEP_INTERVAL = 128,
    API_SWEEP_PROGRESS_SAMPLE_INTERVAL = 2048,
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
    uint64_t sampled_faults;
    uint64_t random_workloads;
    uint64_t invariant_failures;
    uint64_t churn_writes[BENCH_CHURN_CLASS_COUNT];
    uint64_t churn_deletes;
    uint64_t generated_write_bytes;
    uint64_t generated_steps;
    uint64_t truncated_workloads;
};

struct api_sweep_config {
    size_t sector_size;
    size_t sector_count;
    size_t index_sectors;
    enum fffs_sector_size format_sector_size;
};

struct api_sampler_ctx {
    uint64_t allowed_a;
    uint64_t allowed_b;
    const struct api_model *before;
    const struct api_model *after;
    struct api_stats *stats;
    FILE *log;
    const char *log_path;
    struct ffsv_flash *base_flash;
    struct fffs *base_fs;
    bool inspect_internals;
    uint32_t workload_id;
    size_t step_index;
    size_t step_count;
    uint8_t step_type;
    struct dispatcher *dispatcher;
    size_t thread_id;
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
    size_t step_index;
    uint64_t crash_points;
    uint64_t sampled_faults;
    uint64_t failures;
    int err;
};

struct dispatcher {
    pthread_mutex_t mutex;
    uint32_t seed_start;
    size_t seed_count;
    size_t next_seed_index;
    size_t transaction_limit;
    size_t target_write_multiple;
    size_t max_steps;
    size_t thread_count;
    size_t sampled_permutations;
    size_t sampled_max_bits;
    bool inspect_internals;
    struct api_sweep_config flash_config;
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
    void *index_cache;
    uint8_t scratch[API_SWEEP_SCRATCH_SIZE];
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    uint32_t alloc_map[API_SWEEP_ALLOC_MAP_WORDS];
#endif
};

struct api_runtime_state {
    struct ffsv_flash *flash;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file open_file;
    struct mount_storage storage;
    struct open_tx tx;
    bool mounted;
};

static void format_count(char *out, size_t out_size, uint64_t value);
static void publish_worker_progress(struct dispatcher *dispatcher,
        size_t thread_id, uint32_t seed, size_t step_index,
        size_t step_count, const struct api_stats *stats, int err);

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

static uint64_t api_sweep_image_size(const struct api_sweep_config *config) {
    return (uint64_t)config->sector_size * config->sector_count;
}

static size_t api_sweep_max_content(const struct api_sweep_config *config) {
    uint64_t max = api_sweep_image_size(config) * 40u / 100u;
    if (max > UINT16_MAX) {
        max = UINT16_MAX;
    }
    return (size_t)max;
}

static int new_flash(struct ffsv_flash **flash, struct fffs_backend *backend,
        const struct api_sweep_config *config) {
    struct ffsv_flash_config cfg;
    int err = ffsv_flash_config_preset(&cfg, FFSV_PRESET_TARGET_NOR_NOTES,
            (size_t)api_sweep_image_size(config));
    if (err != FFSV_OK) {
        return FFFS_ERR_IO;
    }
    cfg.sector_size = config->sector_size;
    cfg.max_log_entries = 300000;
    err = ffsv_flash_create(flash, &cfg);
    if (err != FFSV_OK) {
        return FFFS_ERR_IO;
    }
    return fffs_host_backend_from_verify_flash(backend, *flash);
}

static int mount_storage_init(struct mount_storage *storage) {
    memset(storage, 0, sizeof(*storage));
    size_t cache_size = API_SWEEP_INDEX_CACHE_SIZE ?
        API_SWEEP_INDEX_CACHE_SIZE : 1u;
    storage->index_cache = calloc(1, cache_size);
    return storage->index_cache ? FFFS_OK : FFFS_ERR_NOMEM;
}

static void mount_storage_destroy(struct mount_storage *storage) {
    free(storage->index_cache);
    *storage = (struct mount_storage){0};
}

static int mount_fs(struct fffs *fs, const struct fffs_backend *backend,
        struct mount_storage *storage) {
    memset(storage->index_cache, 0, API_SWEEP_INDEX_CACHE_SIZE);
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    memset(storage->alloc_map, 0, sizeof(storage->alloc_map));
#endif
    return fffs_mount(fs, backend, &(struct fffs_mount_options){
        .index_cache = storage->index_cache,
        .index_cache_size = API_SWEEP_INDEX_CACHE_SIZE,
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

static int runtime_clone(struct api_runtime_state *dst,
        const struct ffsv_flash *flash, const struct fffs *fs,
        const struct fffs_file *open_file, const struct open_tx *tx,
        const struct api_sweep_config *config) {
    memset(dst, 0, sizeof(*dst));
    int err = mount_storage_init(&dst->storage);
    if (err != FFFS_OK) {
        return err;
    }
    err = ffsv_flash_cow_clone(&dst->flash, flash);
    if (err != FFSV_OK) {
        mount_storage_destroy(&dst->storage);
        return FFFS_ERR_IO;
    }
    err = fffs_host_backend_from_verify_flash(&dst->backend, dst->flash);
    if (err != FFFS_OK) {
        ffsv_flash_destroy(dst->flash);
        mount_storage_destroy(&dst->storage);
        return err;
    }

    dst->fs = *fs;
    dst->fs.backend = dst->backend;
    dst->fs.index_cache = dst->storage.index_cache;
    memcpy(dst->storage.index_cache, fs->index_cache,
            API_SWEEP_INDEX_CACHE_SIZE);
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
    dst->fs.index_heads = dst->storage.index_cache;
#else
    dst->fs.index_heads = NULL;
#endif
    dst->fs.scratch = dst->storage.scratch;
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    memcpy(dst->storage.alloc_map, fs->alloc_map,
            sizeof(dst->storage.alloc_map));
    dst->fs.alloc_map = dst->storage.alloc_map;
#endif

    dst->tx = *tx;
    dst->tx.data = malloc(api_sweep_max_content(config));
    if (!dst->tx.data) {
        ffsv_flash_destroy(dst->flash);
        mount_storage_destroy(&dst->storage);
        return FFFS_ERR_NOMEM;
    }
    if (tx->data && tx->active) {
        memcpy(dst->tx.data, tx->data, tx->size);
    }
    if (tx->active) {
        dst->open_file = *open_file;
        dst->open_file.fs = &dst->fs;
        dst->open_file.inflight_next = NULL;
        dst->fs.inflight_writers = dst->open_file.inflight_registered ?
            &dst->open_file : NULL;
    } else {
        dst->fs.inflight_writers = NULL;
    }
    dst->mounted = true;
    return FFFS_OK;
}

static void runtime_destroy(struct api_runtime_state *state) {
    if (!state) {
        return;
    }
    if (state->mounted) {
        fffs_unmount(&state->fs);
    }
    ffsv_flash_destroy(state->flash);
    mount_storage_destroy(&state->storage);
    free(state->tx.data);
    *state = (struct api_runtime_state){0};
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

static void log_model(FILE *out, const char *label,
        const struct api_model *model) {
    if (!out || !model) {
        return;
    }
    fprintf(out, "%s hash=%016llx", label,
            (unsigned long long)model_hash(model));
    for (size_t i = 0; i < API_SWEEP_FILE_COUNT; i++) {
        const struct model_file *file = &model->files[i];
        if (file->exists) {
            fprintf(out, " %s(size=%zu,tx=%u,file=%u)", file->name,
                    file->size, (unsigned)file->tx_id,
                    (unsigned)file->file_id);
        }
    }
    fprintf(out, "\n");
}

static void log_namespace(FILE *out, struct fffs *fs) {
    if (!out || !fs) {
        return;
    }
    struct fffs_stat entries[API_SWEEP_FILE_COUNT + 8];
    size_t count = 0;
    int err = fffs_list(fs, entries, sizeof(entries) / sizeof(entries[0]),
            &count);
    fprintf(out, "actual list err=%s count=%zu", fffs_status_name(err),
            count);
    if (err != FFFS_OK) {
        fprintf(out, "\n");
        return;
    }
    if (count > sizeof(entries) / sizeof(entries[0])) {
        fprintf(out, " overflow\n");
        return;
    }
    qsort(entries, count, sizeof(entries[0]), stat_name_cmp);
    uint64_t actual = 0;
    err = namespace_hash(fs, &actual);
    fprintf(out, " hash=%s/%016llx", fffs_status_name(err),
            (unsigned long long)actual);
    for (size_t i = 0; i < count; i++) {
        fprintf(out, " %s(size=%u)", entries[i].name,
                (unsigned)entries[i].size);
    }
    fprintf(out, "\n");
}

static bool find_op_record(const struct ffsv_flash *flash, uint64_t sequence,
        struct ffsv_op_record *out) {
    size_t count = 0;
    const struct ffsv_op_record *records = ffsv_flash_log(flash, &count);
    for (size_t i = 0; i < count; i++) {
        if (records[i].sequence == sequence) {
            if (out) {
                *out = records[i];
            }
            return true;
        }
    }
    return false;
}

static void log_inspect_summary(FILE *out, struct fffs_backend *backend) {
    struct fffs_inspect_summary summary = {0};
    int err = fffs_inspect_check(backend, &summary);
    fprintf(out,
            "inspect err=%s index=%zu deletes=%zu live=%zu "
            "live_corrupt=%zu data_corrupt=%zu md_corrupt=%zu "
            "md_live=%zu md_orphaned=%zu md_tombstoned=%zu\n",
            fffs_status_name(err), summary.index_corrupt_records,
            summary.index_deletes, summary.live_entries,
            summary.live_entries_corrupt, summary.data_sectors_corrupt,
            summary.md_corrupt, summary.md_live,
            summary.md_obsolete_orphaned, summary.md_tombstoned);
}

static void log_failure_image(FILE *out, struct fffs_backend *backend,
        struct fffs *fs, uint64_t allowed_a, uint64_t allowed_b) {
    log_inspect_summary(out, backend);
    if (fs) {
        uint64_t actual = 0;
        int hash_err = namespace_hash(fs, &actual);
        fprintf(out, "allowed=%016llx/%016llx actual=%s/%016llx\n",
                (unsigned long long)allowed_a,
                (unsigned long long)allowed_b,
                fffs_status_name(hash_err), (unsigned long long)actual);
        log_namespace(out, fs);
    } else {
        fprintf(out, "allowed=%016llx/%016llx actual=not-mounted\n",
                (unsigned long long)allowed_a,
                (unsigned long long)allowed_b);
    }
    (void)fffs_inspect_dump(backend, out);
}

static void dump_failure_images(FILE *out, const char *log_path,
        const char *kind, uint32_t workload_id, size_t step_index,
        uint64_t sequence, size_t permutation, struct ffsv_flash *before,
        struct ffsv_flash *after) {
    if (!log_path || !before || !after) {
        return;
    }

    char before_path[512];
    char after_path[512];
    snprintf(before_path, sizeof(before_path),
            "%s.%s-%08x-step%zu-seq%llu-perm%zu-before.img",
            log_path, kind, (unsigned)workload_id, step_index,
            (unsigned long long)sequence, permutation);
    snprintf(after_path, sizeof(after_path),
            "%s.%s-%08x-step%zu-seq%llu-perm%zu-after.img",
            log_path, kind, (unsigned)workload_id, step_index,
            (unsigned long long)sequence, permutation);

    int before_err = ffsv_flash_dump_image(before, before_path);
    int after_err = ffsv_flash_dump_image(after, after_path);
    if (out) {
        fprintf(out, "failure images before=%s err=%s after=%s err=%s\n",
                before_path, fffs_status_name(before_err),
                after_path, fffs_status_name(after_err));
    }
}

struct original_index_diag_ctx {
    FILE *out;
    uint16_t sector;
};

static const char *md_record_state_name(const struct fffs_md_record *record) {
    if (record->live) {
        return "live";
    }

    enum fffs_bitmirror_state valid = fffs_lifecycle_valid_pair(record->state);
    enum fffs_bitmirror_state tombstone =
        fffs_lifecycle_tombstone_pair(record->state);
    if (valid == FFFS_BITMIRROR_CLEARED &&
            tombstone == FFFS_BITMIRROR_CLEARED) {
        return "tombstoned";
    }
    if (tombstone == FFFS_BITMIRROR_MIXED) {
        return "partial-tombstone";
    }
    return "not-live";
}

static bool original_sector_reachable(struct fffs *fs, uint16_t slot,
        uint16_t head, uint16_t sector, int *err_out) {
    uint16_t current = head;
    *err_out = FFFS_OK;
    for (size_t depth = 0; current != 0 && depth < fs->sector_count; depth++) {
        struct fffs_md_record record;
        int err = fffs_read_md_for_slot(fs, current, slot, &record);
        if (err != FFFS_OK) {
            *err_out = err;
            return false;
        }
        if (sector >= current && sector < (size_t)current + record.span_len) {
            return true;
        }
        current = record.next;
    }
    if (current != 0) {
        *err_out = FFFS_ERR_CORRUPT;
    }
    return false;
}

static int original_index_diag_visitor(struct fffs *fs,
        const struct fffs_md_record *record, void *user) {
    struct original_index_diag_ctx *ctx = user;
    uint16_t head = 0;
    bool found = false;
    int head_err = fffs_index_head_for_slot(fs, record->slot, &head, &found);

    struct fffs_stat md_st = {0};
    uint16_t next = record->next;
    uint16_t span_len = record->span_len;
    int md_err = FFFS_OK;
    if (record->type == FFFS_MD_TYPE_FILE_ROOT_V1) {
        md_err = fffs_read_file_root_md(fs, ctx->sector, record->slot,
                &md_st, NULL, NULL, NULL, NULL, NULL, NULL);
    }

    bool exists = false;
    int exists_err = md_err == FFFS_OK ?
        fffs_exists(fs, md_st.name, &exists) : md_err;

    struct fffs_stat stat_st = {0};
    int stat_err = md_err == FFFS_OK ?
        fffs_stat(fs, md_st.name, &stat_st) : md_err;

    int reach_err = FFFS_OK;
    bool reachable = false;
    if (head_err == FFFS_OK && found) {
        reachable = original_sector_reachable(fs, record->slot, head,
                ctx->sector, &reach_err);
    }

    fprintf(ctx->out,
            "original-index sector=%u record_off=%u lifecycle=%s "
            "slot=%u md_next=%u md_span=%u md_name=%s md_size=%u md_err=%s "
            "head_found=%d head=%u head_err=%s reachable=%d "
            "reach_err=%s exists=%d exists_err=%s stat_size=%u "
            "stat_err=%s\n",
            (unsigned)ctx->sector, (unsigned)record->record_start,
            md_record_state_name(record), (unsigned)record->slot,
            (unsigned)next, (unsigned)span_len,
            md_err == FFFS_OK ? md_st.name : "",
            (unsigned)md_st.size, fffs_status_name(md_err), found,
            (unsigned)head, fffs_status_name(head_err), reachable,
            fffs_status_name(reach_err), exists, fffs_status_name(exists_err),
            (unsigned)stat_st.size, fffs_status_name(stat_err));
    return FFFS_OK;
}

static void log_original_index_view(FILE *out, struct fffs *fs,
        const struct ffsv_fault_case *fault) {
    if (!out || !fs || !fault || fs->sector_size == 0) {
        return;
    }
    uint16_t sector = (uint16_t)(fault->offset / fs->sector_size);
    if (sector < fs->index_sectors || sector >= fs->sector_count) {
        return;
    }
    fprintf(out, "original-index sector=%u fault_offset=%zu\n",
            (unsigned)sector, fault->offset);
    struct original_index_diag_ctx ctx = {
        .out = out,
        .sector = sector,
    };
    uint8_t window_buf[FFFS_MD_PRELOAD_MAX];
    struct fffs_sector_reader window = {
        .data = window_buf,
        .capacity = sizeof(window_buf),
        .reverse = true,
    };
    const uint8_t *footer_bytes;
    int err = fffs_sector_reader_view(fs, &window, sector,
            fs->sector_size - FFFS_SECTOR_FOOTER_SIZE,
            FFFS_SECTOR_FOOTER_SIZE, &footer_bytes);
    if (err == FFFS_OK) {
        struct fffs_sector_footer footer;
        fffs_decode_sector_footer(footer_bytes, &footer);
        if (footer.erased || footer.type != FFFS_SECTOR_TYPE_FILE ||
                !footer.magic_valid ||
                !fffs_lifecycle_is_live(footer.valid_bits,
                    footer.tombstone_bits)) {
            fprintf(out, "original-index visit_err=%s\n",
                    fffs_status_name(FFFS_OK));
            return;
        }
        struct fffs_md_walk walk;
        err = fffs_md_walk_init(fs, &walk, sector, &window);
        while (err == FFFS_OK && walk.active) {
            struct fffs_md_record record;
            enum fffs_md_walk_result result;
            err = fffs_md_walk_next(fs, &walk, &window, &record, &result);
            if (err != FFFS_OK || result != FFFS_MD_WALK_RECORD) {
                break;
            }
            err = original_index_diag_visitor(fs, &record, &ctx);
        }
    }
    fprintf(out, "original-index visit_err=%s\n", fffs_status_name(err));
}

static int check_image_ex(struct fffs_backend *backend, struct fffs *fs,
        uint64_t allowed_a, uint64_t allowed_b, bool allow_raw_corrupt,
        bool inspect_internals) {
    if (inspect_internals) {
        struct fffs_inspect_summary summary;
        int err = fffs_inspect_check(backend, &summary);
        if (err != FFFS_OK) {
            return err;
        }
        if (!allow_raw_corrupt && (summary.index_corrupt_records ||
                summary.live_entries_corrupt ||
                summary.data_sectors_corrupt || summary.md_corrupt)) {
            return FFFS_ERR_CORRUPT;
        }
    }
    uint64_t actual;
    int err = namespace_hash(fs, &actual);
    if (err != FFFS_OK) {
        return err;
    }
    return actual == allowed_a || actual == allowed_b ? FFFS_OK :
        FFFS_ERR_CORRUPT;
}

static int verify_sampled_fault(const struct ffsv_fault_case *fault,
        void *user) {
    struct api_sampler_ctx *ctx = user;
    struct fffs_backend backend;
    struct fffs fs;
    struct mount_storage storage;

    int err = mount_storage_init(&storage);
    if (err != FFFS_OK) {
        return FFSV_ERR_NOMEM;
    }
    err = fffs_host_backend_from_verify_flash(&backend, fault->branch);
    if (err == FFFS_OK) {
        err = mount_fs(&fs, &backend, &storage);
    }
    bool mounted = err == FFFS_OK;
    if (err == FFFS_OK) {
        err = check_image_ex(&backend, &fs, ctx->allowed_a, ctx->allowed_b,
                true, ctx->inspect_internals);
    }

    ctx->stats->sampled_faults++;
    ctx->stats->crash_points++;
    if (ctx->dispatcher &&
            (ctx->stats->sampled_faults %
                API_SWEEP_PROGRESS_SAMPLE_INTERVAL) == 0) {
        publish_worker_progress(ctx->dispatcher, ctx->thread_id,
                ctx->workload_id, ctx->step_index, ctx->step_count,
                ctx->stats, FFFS_OK);
    }
    if (err != FFFS_OK) {
        ctx->stats->invariant_failures++;
        publish_worker_progress(ctx->dispatcher, ctx->thread_id,
                ctx->workload_id, ctx->step_index, ctx->step_count,
                ctx->stats, err);
        FILE *out = ctx->log ? ctx->log : stderr;
        fprintf(out,
                "api sampled failure workload=%u step=%zu type=%u "
                "seq=%llu op=%s offset=%zu size=%zu perm=%zu seed=%u "
                "bits=%zu mounted=%d err=%s\n",
                (unsigned)ctx->workload_id, ctx->step_index,
                (unsigned)ctx->step_type, (unsigned long long)fault->sequence,
                ffsv_op_name(fault->type), fault->offset, fault->size,
                fault->permutation, (unsigned)fault->seed,
                fault->sampled_bits, mounted, fffs_status_name(err));
        log_model(out, "expected-before", ctx->before);
        log_model(out, "expected-after", ctx->after);
        log_original_index_view(out, ctx->base_fs, fault);
        dump_failure_images(out, ctx->log_path, "sampled",
                ctx->workload_id, ctx->step_index, fault->sequence,
                fault->permutation, ctx->base_flash, fault->branch);
        log_failure_image(out, &backend, mounted ? &fs : NULL,
                ctx->allowed_a, ctx->allowed_b);
        fflush(out);
        if (mounted) {
            fffs_unmount(&fs);
        }
        mount_storage_destroy(&storage);
        return FFSV_ERR_IO;
    }
    if (mounted) {
        fffs_unmount(&fs);
    }
    mount_storage_destroy(&storage);
    return FFSV_OK;
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

static int verify_cow_crash_point(const struct api_runtime_state *base,
        const struct api_step *step, const struct crash_point *point,
        FILE *log, const char *log_path,
        const struct api_sweep_config *config, bool inspect_internals) {
    struct api_runtime_state attempt;
    int err = runtime_clone(&attempt, base->flash, &base->fs,
            &base->open_file, &base->tx, config);
    if (err != FFFS_OK) {
        return err;
    }

    ffsv_flash_set_failure(attempt.flash, &(struct ffsv_failure_injection){
        .enabled = true,
        .sequence = point->sequence,
        .op_mask = (UINT32_C(1) << FFSV_OP_PROGRAM) |
            (UINT32_C(1) << FFSV_OP_ERASE),
        .phase = FFSV_FAIL_AFTER,
        .status = FFSV_ERR_INJECTED,
    });
    err = apply_step(&attempt.fs, step, &attempt.open_file, &attempt.tx);
    ffsv_flash_clear_failure(attempt.flash);
    bool injected = log_has_injected_sequence(attempt.flash,
            point->sequence);
    struct ffsv_op_record injected_record = {0};
    bool have_record = find_op_record(attempt.flash, point->sequence,
            &injected_record);

    if (!injected) {
        err = FFFS_ERR_CORRUPT;
    } else {
        err = FFFS_OK;
    }

    if (err == FFFS_OK) {
        fffs_unmount(&attempt.fs);
        attempt.mounted = false;

        struct fffs recovered;
        struct mount_storage storage;
        err = mount_storage_init(&storage);
        if (err == FFFS_OK) {
            err = mount_fs(&recovered, &attempt.backend, &storage);
        }
        if (err == FFFS_OK) {
            err = check_image_ex(&attempt.backend, &recovered,
                    point->allowed_a, point->allowed_b, true,
                    inspect_internals);
            if (err != FFFS_OK && log) {
                fprintf(log,
                        "api crash diagnostic workload=%u step=%u type=%u "
                        "seq=%llu err=%s\n",
                        point->workload_id, (unsigned)point->step_index,
                        (unsigned)point->step_type,
                        (unsigned long long)point->sequence,
                        fffs_status_name(err));
                if (have_record) {
                    fprintf(log,
                            "op type=%s offset=%zu size=%zu result=%s "
                            "injected=%d phase=%d committed=%zu\n",
                            ffsv_op_name(injected_record.type),
                            injected_record.offset, injected_record.size,
                            fffs_status_name(injected_record.result),
                            injected_record.injected,
                            injected_record.injected_phase,
                            injected_record.committed_bytes);
                }
                dump_failure_images(log, log_path, "crash",
                        point->workload_id, point->step_index,
                        point->sequence, 0, base->flash, attempt.flash);
                log_failure_image(log, &attempt.backend, &recovered,
                        point->allowed_a, point->allowed_b);
            }
            fffs_unmount(&recovered);
        }
        mount_storage_destroy(&storage);
    }

    runtime_destroy(&attempt);
    return err;
}

static int run_steps(struct ffsv_flash *flash, struct fffs_backend *backend,
        const struct api_step *steps, size_t step_count,
        struct crash_point *points, size_t *point_count, uint32_t workload_id,
        struct api_stats *stats,
        size_t sampled_permutations, size_t sampled_max_bits, FILE *log,
        const char *log_path, struct dispatcher *dispatcher,
        size_t thread_id, const struct api_sweep_config *config,
        bool inspect_internals) {
    struct fffs fs;
    struct fffs_file open_file;
    struct mount_storage storage;
    struct api_model model = {0};
    struct open_tx tx = {0};
    tx.data = malloc(api_sweep_max_content(config));
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

    for (size_t i = 0; i < step_count; i++) {
        if (dispatcher && (i % API_SWEEP_PROGRESS_STEP_INTERVAL) == 0) {
            publish_worker_progress(dispatcher, thread_id, workload_id, i,
                    step_count, stats, FFFS_OK);
        }
        struct api_model before_model = model;
        struct api_model after_model = model;
        struct open_tx before_tx = tx;
        model_apply_step(&after_model, &steps[i], &before_tx);
        uint64_t before_hash = model_hash(&before_model);
        uint64_t after_hash = model_hash(&after_model);
        size_t log_before = 0;
        (void)ffsv_flash_log(flash, &log_before);

        struct api_runtime_state base = {0};
        err = runtime_clone(&base, flash, &fs, &open_file, &tx, config);
        if (err != FFFS_OK) {
            break;
        }

        struct api_sampler_ctx sampler_ctx = {0};
        if (sampled_permutations) {
            sampler_ctx = (struct api_sampler_ctx){
                .allowed_a = before_hash,
                .allowed_b = after_hash,
                .before = &before_model,
                .after = &after_model,
                .stats = stats,
                .log = log,
                .log_path = log_path,
                .base_flash = flash,
                .base_fs = &fs,
                .inspect_internals = inspect_internals,
                .workload_id = workload_id,
                .step_index = i,
                .step_count = step_count,
                .step_type = (uint8_t)steps[i].type,
                .dispatcher = dispatcher,
                .thread_id = thread_id,
            };
            ffsv_flash_set_fault_sampler(flash, &(struct ffsv_fault_sampler){
                .enabled = true,
                .op_mask = (UINT32_C(1) << FFSV_OP_PROGRAM) |
                    (UINT32_C(1) << FFSV_OP_COMMIT_STAGED) |
                    (UINT32_C(1) << FFSV_OP_ERASE),
                .seed = UINT32_C(0xa7150000) ^ workload_id ^
                    (uint32_t)(i * 131u),
                .permutations_per_op = sampled_permutations,
                .max_bits_per_permutation = sampled_max_bits,
                .program_page_size = 256,
                .verify = verify_sampled_fault,
                .user = &sampler_ctx,
            });
        }
        err = apply_step(&fs, &steps[i], &open_file, &tx);
        ffsv_flash_clear_fault_sampler(flash);
        if (err != FFFS_OK) {
            if (log) {
                fprintf(log,
                        "api apply failure workload=%u step=%zu type=%u "
                        "err=%s\n",
                        workload_id, i, (unsigned)steps[i].type,
                        fffs_status_name(err));
                log_model(log, "expected-before", &before_model);
                log_model(log, "expected-after", &after_model);
                dump_failure_images(log, log_path, "apply",
                        workload_id, i, 0, 0, base.flash, flash);
                fprintf(log, "apply-before image diagnostics\n");
                log_failure_image(log, &base.backend, &base.fs,
                        before_hash, after_hash);
                fprintf(log, "apply-after image diagnostics\n");
                log_failure_image(log, backend, &fs, before_hash,
                        after_hash);
                fflush(log);
            }
            runtime_destroy(&base);
            break;
        }
        size_t log_count = 0;
        const struct ffsv_op_record *records = ffsv_flash_log(flash,
                &log_count);
        size_t point_start = *point_count;
        err = collect_crash_points(records, log_before, log_count, points,
                point_count, before_hash, after_hash, workload_id,
                (uint16_t)i, (uint8_t)steps[i].type);
        if (err != FFFS_OK) {
            runtime_destroy(&base);
            break;
        }
        for (size_t p = point_start; p < *point_count; p++) {
            err = verify_cow_crash_point(&base, &steps[i], &points[p], log,
                    log_path, config, inspect_internals);
            stats->crash_points++;
            if (err != FFFS_OK) {
                stats->invariant_failures++;
                FILE *out = log ? log : stderr;
                fprintf(out,
                        "api crash failure workload=%u step=%u type=%u "
                        "seq=%llu err=%s\n",
                        points[p].workload_id,
                        (unsigned)points[p].step_index,
                        (unsigned)points[p].step_type,
                        (unsigned long long)points[p].sequence,
                        fffs_status_name(err));
                fflush(out);
                break;
            }
        }
        runtime_destroy(&base);
        if (err != FFFS_OK) {
            break;
        }
        if (err == FFFS_OK) {
            model = after_model;
        }
    }
    if (dispatcher) {
        publish_worker_progress(dispatcher, thread_id, workload_id,
                step_count, step_count, stats, err);
    }

    fffs_unmount(&fs);
    mount_storage_destroy(&storage);
    free(tx.data);
    return err;
}

static int init_formatted_flash(struct ffsv_flash **flash,
        struct fffs_backend *backend,
        const struct api_sweep_config *config) {
    int err = new_flash(flash, backend, config);
    if (err != FFFS_OK) {
        return err;
    }
    return fffs_format(backend, &(struct fffs_format_options){
        .index_sectors = (uint8_t)config->index_sectors,
        .sector_size = config->format_sector_size,
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
        uint32_t workload_id, FILE *log, struct wear_stats *wear_stats,
        struct api_stats *stats, size_t sampled_permutations,
        size_t sampled_max_bits, struct dispatcher *dispatcher,
        size_t thread_id, const char *log_path,
        const struct api_sweep_config *config, bool inspect_internals) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    int err = init_formatted_flash(&flash, &backend, config);
    if (err == FFFS_OK) {
        err = run_steps(flash, &backend, steps, step_count, points,
                point_count, workload_id, stats, sampled_permutations,
                sampled_max_bits, log, log_path, dispatcher, thread_id,
                config, inspect_internals);
        if (err == FFFS_OK) {
            log_flash_wear(log, flash, workload_id, wear_stats);
        }
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
    char cases[32];
    char sampled[32];
    char failures[32];
    format_count(cases, sizeof(cases), status->crash_points);
    format_count(sampled, sizeof(sampled), status->sampled_faults);
    snprintf(failures, sizeof(failures), "%llu",
            (unsigned long long)status->failures);
    size_t used = strlen(out);
    if (used >= out_size) {
        return;
    }
    const char *sep = used == 0 ? "" : " ";
    if (status->active) {
        (void)id;
        snprintf(out + used, out_size - used,
                "%s%08x:%zu/%zu:c%s:p%s:f%s",
                sep, (unsigned)status->seed, status->step_index,
                status->steps, cases, sampled, failures);
    } else {
        (void)id;
        snprintf(out + used, out_size - used, "%s%s:c%s:p%s:f%s",
                sep, status->done ? "done" : "idle", cases, sampled,
                failures);
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

static void publish_worker_progress(struct dispatcher *dispatcher,
        size_t thread_id, uint32_t seed, size_t step_index,
        size_t step_count, const struct api_stats *stats, int err) {
    if (!dispatcher || !stats) {
        return;
    }
    struct worker_status status = {
        .active = true,
        .seed = seed,
        .steps = step_count,
        .step_index = step_index,
        .crash_points = stats->crash_points,
        .sampled_faults = stats->sampled_faults,
        .failures = stats->invariant_failures,
        .err = err,
    };
    update_worker_status(dispatcher, thread_id, &status);
    maybe_report_worker_status(dispatcher);
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

static size_t scaled_churn_size(const bench_churn_event_t *event,
        const struct api_sweep_config *config) {
    size_t max_content = api_sweep_max_content(config);
    size_t large_min = max_content * 3u / 4u;
    size_t large_span = max_content - large_min + 1u;
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

static size_t api_sweep_file_data_per_sector(
        const struct api_sweep_config *config) {
    size_t raw = config->sector_size - FFFS_SECTOR_FOOTER_SIZE -
        FFFS_MD_SIZE;
    return raw - (raw % API_SWEEP_PROGRAM_GRANULE);
}

static size_t api_sweep_file_sector_count(size_t payload_size,
        const struct api_sweep_config *config) {
    size_t data_per_sector = api_sweep_file_data_per_sector(config);
    if (payload_size == 0) {
        return 1;
    }
    return (payload_size + data_per_sector - 1u) / data_per_sector;
}

static size_t estimated_file_footprint(size_t payload_size,
        const struct api_sweep_config *config) {
    return api_sweep_file_sector_count(payload_size, config) *
        config->sector_size;
}

static size_t churn_workload(uint32_t seed, struct api_step *steps,
        size_t capacity, size_t tx_count, size_t target_write_multiple,
        const struct api_sweep_config *config, struct api_stats *stats) {
    bench_churn_model_t model;
    uint32_t rng = seed ^ UINT32_C(0x5eed1234);
    size_t pos = 0;
    size_t tx = 0;
    uint64_t actual_written = 0;
    uint64_t image_size = api_sweep_image_size(config);
    uint64_t target_written64 = image_size * target_write_multiple;
    uint32_t force_large_after = target_written64 / 10u > UINT32_MAX ?
        UINT32_MAX : (uint32_t)(target_written64 / 10u);
    if (force_large_after < 20u * 1024u) {
        force_large_after = 20u * 1024u;
    }

    bench_churn_model_init(&model, seed,
            (uint32_t)(image_size * 75u / 100u),
            UINT32_MAX, (uint32_t)(image_size / 8u),
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
        size_t size = scaled_churn_size(&event, config);
        size_t max_content = api_sweep_max_content(config);
        if (size > max_content) {
            size = max_content;
        }
        size_t model_size = estimated_file_footprint(size, config);
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
        struct wear_stats *wear_stats, const char *log_path,
        const struct api_sweep_config *config, bool inspect_internals) {
    struct crash_point *points = calloc(API_SWEEP_MAX_CRASH_POINTS,
            sizeof(*points));
    if (!points) {
        return FFFS_ERR_NOMEM;
    }
    size_t point_count = 0;
    int err = collect_workload_points(steps, step_count, points,
            &point_count, workload_id, log, wear_stats, stats,
            dispatcher ? dispatcher->sampled_permutations : 0,
            dispatcher ? dispatcher->sampled_max_bits : 0,
            dispatcher, thread_id, log_path, config, inspect_internals);
    if (log) {
        fprintf(log, "seed=0x%08x crash_points=%zu\n",
                (unsigned)workload_id, point_count);
        fflush(log);
    }
    if (dispatcher) {
        struct worker_status status = {
            .active = true,
            .seed = workload_id,
            .steps = step_count,
            .step_index = step_count,
            .crash_points = stats->crash_points,
            .sampled_faults = stats->sampled_faults,
            .failures = stats->invariant_failures,
            .err = err,
        };
        update_worker_status(dispatcher, thread_id, &status);
        maybe_report_worker_status(dispatcher);
    }
    free(points);
    return err;
}

static void stats_add(struct api_stats *dst, const struct api_stats *src) {
    dst->crash_points += src->crash_points;
    dst->sampled_faults += src->sampled_faults;
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
                dispatcher->transaction_limit,
                dispatcher->target_write_multiple, &dispatcher->flash_config,
                &ctx->stats);
        struct worker_status status = {
            .active = true,
            .seed = seed,
            .steps = count,
            .step_index = 0,
            .crash_points = ctx->stats.crash_points,
            .sampled_faults = ctx->stats.sampled_faults,
            .failures = ctx->stats.invariant_failures,
            .err = FFFS_OK,
        };
        update_worker_status(dispatcher, ctx->thread_id, &status);
        if (ctx->log) {
            fprintf(ctx->log,
                    "thread=%zu seed_index=%zu seed=0x%08x steps=%zu "
                    "transaction_limit=%zu\n",
                    ctx->thread_id, seed_index, (unsigned)seed, count,
                    dispatcher->transaction_limit);
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
                dispatcher, ctx->thread_id, &ctx->last_wear,
                ctx->log_path, &dispatcher->flash_config,
                dispatcher->inspect_internals);
        last_err = err;
        ctx->stats.random_workloads++;
        status.active = false;
        status.done = false;
        status.crash_points = ctx->stats.crash_points;
        status.sampled_faults = ctx->stats.sampled_faults;
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
        .sampled_faults = ctx->stats.sampled_faults,
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
        size_t transaction_limit, size_t target_write_multiple,
        size_t thread_count, size_t sampled_permutations,
        size_t sampled_max_bits, const char *log_path,
        const struct api_sweep_config *flash_config, struct api_stats *stats,
        struct wear_summary *wear_summary, bool inspect_internals) {
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
        .transaction_limit = transaction_limit,
        .target_write_multiple = target_write_multiple,
        .max_steps = API_SWEEP_DEFAULT_MAX_STEPS,
        .thread_count = thread_count,
        .sampled_permutations = sampled_permutations,
        .sampled_max_bits = sampled_max_bits,
        .inspect_internals = inspect_internals,
        .flash_config = *flash_config,
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

struct cli_options {
    uint32_t seed_start;
    size_t run_count;
    size_t transaction_limit;
    size_t target_write_multiple;
    size_t thread_count;
    const char *log_path;
    size_t partial_write_samples;
    size_t partial_write_bits;
    bool inspect_internals;
    struct api_sweep_config flash_config;
};

static void default_log_path(char *out, size_t out_size) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm && strftime(out, out_size,
                "fffs_api_crash_sweep-%Y%m%d-%H%M%S", tm) != 0) {
        return;
    }
    snprintf(out, out_size, "fffs_api_crash_sweep");
}

static int parse_size_value(const char *text, size_t *out) {
    if (!text || !*text) {
        return FFFS_ERR_INVALID;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno || !end || *end != '\0' || value > SIZE_MAX) {
        return FFFS_ERR_INVALID;
    }
    *out = (size_t)value;
    return FFFS_OK;
}

static int parse_u32_value(const char *text, uint32_t *out) {
    size_t value = 0;
    int err = parse_size_value(text, &value);
    if (err != FFFS_OK || value > UINT32_MAX) {
        return FFFS_ERR_INVALID;
    }
    *out = (uint32_t)value;
    return FFFS_OK;
}

static bool sector_size_to_format(size_t size,
        enum fffs_sector_size *format_size) {
    switch (size) {
    case FFFS_SECTOR_256:
    case FFFS_SECTOR_512:
    case FFFS_SECTOR_1K:
    case FFFS_SECTOR_2K:
    case FFFS_SECTOR_4K:
    case FFFS_SECTOR_8K:
        *format_size = (enum fffs_sector_size)size;
        return true;
    default:
        return false;
    }
}

static int validate_flash_config(struct api_sweep_config *config) {
    if (!sector_size_to_format(config->sector_size,
                &config->format_sector_size)) {
        return FFFS_ERR_INVALID;
    }
    if (config->sector_count == 0 || config->index_sectors == 0 ||
            config->index_sectors >= config->sector_count ||
            config->index_sectors > UINT8_MAX) {
        return FFFS_ERR_INVALID;
    }
    if (config->sector_count > SIZE_MAX / config->sector_size ||
            api_sweep_image_size(config) > UINT32_MAX) {
        return FFFS_ERR_INVALID;
    }
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    if ((config->sector_count + 31u) / 32u > API_SWEEP_ALLOC_MAP_WORDS) {
        return FFFS_ERR_INVALID;
    }
#endif
    if (api_sweep_file_data_per_sector(config) == 0 ||
            api_sweep_max_content(config) == 0) {
        return FFFS_ERR_INVALID;
    }
    return FFFS_OK;
}

static const char *option_value(int argc, char **argv, int *index,
        const char *arg, const char *long_name) {
    size_t long_len = strlen(long_name);
    if (strncmp(arg, "--", 2) == 0 &&
            strncmp(arg, long_name, long_len) == 0 &&
            arg[long_len] == '=') {
        return arg + long_len + 1u;
    }
    if (*index + 1 >= argc) {
        return NULL;
    }
    *index += 1;
    return argv[*index];
}

static bool is_long_option(const char *arg, const char *long_name) {
    size_t long_len = strlen(long_name);
    return strcmp(arg, long_name) == 0 ||
        (strncmp(arg, long_name, long_len) == 0 && arg[long_len] == '=');
}

static int parse_cli(int argc, char **argv, struct cli_options *opts,
        char *default_log, size_t default_log_size) {
    default_log_path(default_log, default_log_size);
    *opts = (struct cli_options){
        .seed_start = API_SWEEP_DEFAULT_SEED,
        .run_count = API_SWEEP_DEFAULT_RUNS,
        .transaction_limit = SIZE_MAX,
        .target_write_multiple = API_SWEEP_DEFAULT_WRITE_MULTIPLE,
        .thread_count = API_SWEEP_DEFAULT_THREADS,
        .log_path = default_log,
        .partial_write_samples = API_SWEEP_DEFAULT_PARTIAL_WRITE_SAMPLES,
        .partial_write_bits = API_SWEEP_DEFAULT_PARTIAL_WRITE_BITS,
        .inspect_internals = true,
        .flash_config = {
            .sector_size = API_SWEEP_DEFAULT_SECTOR_SIZE,
            .sector_count = API_SWEEP_DEFAULT_SECTOR_COUNT,
            .index_sectors = API_SWEEP_DEFAULT_INDEX_SECTORS,
            .format_sector_size = FFFS_SECTOR_256,
        },
    };

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = NULL;
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            return 1;
        } else if (strcmp(arg, "-s") == 0 ||
                is_long_option(arg, "--seed")) {
            value = option_value(argc, argv, &i, arg, "--seed");
            if (!value || parse_u32_value(value, &opts->seed_start) !=
                    FFFS_OK) {
                return FFFS_ERR_INVALID;
            }
        } else if (strcmp(arg, "-n") == 0 ||
                is_long_option(arg, "--runs")) {
            value = option_value(argc, argv, &i, arg, "--runs");
            if (!value || parse_size_value(value, &opts->run_count) !=
                    FFFS_OK || opts->run_count == 0) {
                return FFFS_ERR_INVALID;
            }
        } else if (strcmp(arg, "-w") == 0 ||
                is_long_option(arg, "--write-multiple")) {
            value = option_value(argc, argv, &i, arg, "--write-multiple");
            if (!value || parse_size_value(value,
                        &opts->target_write_multiple) != FFFS_OK ||
                    opts->target_write_multiple == 0) {
                return FFFS_ERR_INVALID;
            }
        } else if (strcmp(arg, "-t") == 0 ||
                is_long_option(arg, "--transactions")) {
            value = option_value(argc, argv, &i, arg, "--transactions");
            if (!value || parse_size_value(value,
                        &opts->transaction_limit) != FFFS_OK ||
                    opts->transaction_limit == 0) {
                return FFFS_ERR_INVALID;
            }
        } else if (strcmp(arg, "-j") == 0 ||
                is_long_option(arg, "--threads")) {
            value = option_value(argc, argv, &i, arg, "--threads");
            if (!value || parse_size_value(value, &opts->thread_count) !=
                    FFFS_OK || opts->thread_count == 0 ||
                    opts->thread_count > API_SWEEP_MAX_THREADS) {
                return FFFS_ERR_INVALID;
            }
        } else if (strcmp(arg, "-l") == 0 ||
                is_long_option(arg, "--log-base")) {
            value = option_value(argc, argv, &i, arg, "--log-base");
            if (!value || !*value) {
                return FFFS_ERR_INVALID;
            }
            opts->log_path = value;
        } else if (strcmp(arg, "-p") == 0 ||
                is_long_option(arg, "--partial-write-samples")) {
            value = option_value(argc, argv, &i, arg,
                    "--partial-write-samples");
            if (!value || parse_size_value(value,
                        &opts->partial_write_samples) != FFFS_OK) {
                return FFFS_ERR_INVALID;
            }
        } else if (strcmp(arg, "-b") == 0 ||
                is_long_option(arg, "--partial-write-bits")) {
            value = option_value(argc, argv, &i, arg,
                    "--partial-write-bits");
            if (!value || parse_size_value(value,
                        &opts->partial_write_bits) != FFFS_OK) {
                return FFFS_ERR_INVALID;
            }
        } else if (strcmp(arg, "-S") == 0 ||
                is_long_option(arg, "--sector-size")) {
            value = option_value(argc, argv, &i, arg, "--sector-size");
            if (!value || parse_size_value(value,
                        &opts->flash_config.sector_size) != FFFS_OK) {
                return FFFS_ERR_INVALID;
            }
        } else if (strcmp(arg, "-C") == 0 ||
                is_long_option(arg, "--sector-count")) {
            value = option_value(argc, argv, &i, arg, "--sector-count");
            if (!value || parse_size_value(value,
                        &opts->flash_config.sector_count) != FFFS_OK) {
                return FFFS_ERR_INVALID;
            }
        } else if (strcmp(arg, "-I") == 0 ||
                is_long_option(arg, "--index-sectors")) {
            value = option_value(argc, argv, &i, arg, "--index-sectors");
            if (!value || parse_size_value(value,
                        &opts->flash_config.index_sectors) != FFFS_OK) {
                return FFFS_ERR_INVALID;
            }
        } else if (strcmp(arg, "--namespace-only") == 0 ||
                strcmp(arg, "--no-inspect") == 0) {
            opts->inspect_internals = false;
        } else {
            return FFFS_ERR_INVALID;
        }
    }
    return validate_flash_config(&opts->flash_config);
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
    printf("  %s [options]\n", argv0);
    printf("\n");
    printf("Options:\n");
    printf("  -s, --seed <u32>                  first PRNG seed "
            "(default 0x%08x)\n", (unsigned)API_SWEEP_DEFAULT_SEED);
    printf("  -n, --runs <count>                seed-driven runs "
            "(default %u)\n", (unsigned)API_SWEEP_DEFAULT_RUNS);
    printf("  -w, --write-multiple <count>      stop after writes reach "
            "count * image size (default %u)\n",
            (unsigned)API_SWEEP_DEFAULT_WRITE_MULTIPLE);
    printf("  -t, --transactions <count>        optional transaction cap "
            "per run (default unlimited)\n");
    printf("  -j, --threads <count>             worker thread count "
            "(default %u, max %u)\n", (unsigned)API_SWEEP_DEFAULT_THREADS,
            (unsigned)API_SWEEP_MAX_THREADS);
    printf("  -l, --log-base <path>             base path for per-thread "
            "logs (default timestamped)\n");
    printf("  -p, --partial-write-samples <n>   partial-program fault "
            "samples per mutating flash op (default %u)\n",
            (unsigned)API_SWEEP_DEFAULT_PARTIAL_WRITE_SAMPLES);
    printf("  -b, --partial-write-bits <n>      eligible 1->0 bits to "
            "sample per partial-write case (default %u, 0 means all)\n",
            (unsigned)API_SWEEP_DEFAULT_PARTIAL_WRITE_BITS);
    printf("  -S, --sector-size <bytes>         flash sector size: "
            "256, 512, 1024, 2048, 4096, or 8192 (default %u)\n",
            (unsigned)API_SWEEP_DEFAULT_SECTOR_SIZE);
    printf("  -C, --sector-count <count>        flash sector count "
            "(default %u)\n", (unsigned)API_SWEEP_DEFAULT_SECTOR_COUNT);
    printf("  -I, --index-sectors <count>       FASTFFS index sectors "
            "(default %u)\n", (unsigned)API_SWEEP_DEFAULT_INDEX_SECTORS);
    printf("      --namespace-only              skip internal sector "
            "inspection; still mount, list, and read visible files\n");
    printf("      --no-inspect                  alias for --namespace-only\n");
    printf("  -h, --help                        show this help and exit\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s -s 0x46464653 -n 1 -w 2 -j 1\n", argv0);
    printf("  %s --seed 1234 --runs 4 --write-multiple 10 "
            "--threads 4\n", argv0);
    printf("  %s -s 1234 -n 1 -w 4 -j 1 "
            "-p 10 -b 8 -S 512 -C 512 -I 4 "
            "-l /tmp/fffs_api_diag\n", argv0);
}

int main(int argc, char **argv) {
    struct cli_options opts;
    char default_log[64];
    int cli_err = parse_cli(argc, argv, &opts, default_log,
            sizeof(default_log));
    if (cli_err == 1) {
        print_usage(argv[0]);
        return 0;
    }
    if (cli_err != FFFS_OK) {
        print_usage(argv[0]);
        return 2;
    }
    struct api_stats stats = {0};
    struct wear_summary wear_summary = {0};

    printf("fffs api crash sweep [%s]\n", cache_mode_name());
    printf("logs: %s.t<N>.log\n", opts.log_path);
    int err = run_random(opts.seed_start, opts.run_count,
            opts.transaction_limit, opts.target_write_multiple,
            opts.thread_count, opts.partial_write_samples,
            opts.partial_write_bits, opts.log_path, &opts.flash_config,
            &stats, &wear_summary, opts.inspect_internals);
    printf("seed: 0x%08x\n", (unsigned)opts.seed_start);
    print_count_line("threads", opts.thread_count);
    print_count_line("runs", opts.run_count);
    if (opts.transaction_limit == SIZE_MAX) {
        printf("transaction cap: unlimited\n");
    } else {
        print_count_line("transaction cap", opts.transaction_limit);
    }
    print_count_line("write multiple", opts.target_write_multiple);
    print_count_line("sector size", opts.flash_config.sector_size);
    print_count_line("sector count", opts.flash_config.sector_count);
    print_count_line("index sectors", opts.flash_config.index_sectors);
    print_count_line("image bytes", api_sweep_image_size(&opts.flash_config));
    printf("inspect internals: %s\n",
            opts.inspect_internals ? "yes" : "no");
    print_count_line("partial-write samples", opts.partial_write_samples);
    print_count_line("partial-write bits", opts.partial_write_bits);
    print_count_line("generated API steps", stats.generated_steps);
    print_count_line("generated write bytes", stats.generated_write_bytes);
    print_count_line("truncated workloads", stats.truncated_workloads);
    print_count_line("crash points tested", stats.crash_points);
    print_count_line("sampled fault cases", stats.sampled_faults);
    print_count_line("runs completed", stats.random_workloads);
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
