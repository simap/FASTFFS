#include "fastffs/fastffs.h"
#include "fastffs/fastffs_host.h"
#include "fastffs/fastffs_inspect.h"
#include "fastffs/verify_flash.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
#define SWEEP_INDEX_HEADS FFFS_SLOT_COUNT
#else
#define SWEEP_INDEX_HEADS FFFS_INDEX_HASH_TABLE_SIZE
#endif

enum {
    SWEEP_SECTOR_SIZE = 256,
    SWEEP_SECTOR_COUNT = 48,
    SWEEP_INDEX_SECTORS = 2,
    SWEEP_FILE_COUNT = 6,
    SWEEP_MAX_OPS = 128,
    SWEEP_SCRATCH_SIZE = 4096,
    SWEEP_MAX_MUTATIONS_PER_OP = 256,
};

enum sweep_op_type {
    SWEEP_OP_WRITE = 0,
    SWEEP_OP_DELETE = 1,
};

struct sweep_op {
    enum sweep_op_type type;
    uint8_t file_id;
    uint8_t value;
};

struct sweep_model {
    bool exists[SWEEP_FILE_COUNT];
    uint8_t value[SWEEP_FILE_COUNT];
};

struct sweep_stats {
    uint64_t crash_points;
    uint64_t random_workloads;
    uint64_t tiny_cases;
    uint64_t invariant_failures;
};

struct mount_storage {
    uint16_t *index_heads;
    uint8_t scratch[SWEEP_SCRATCH_SIZE];
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    uint32_t alloc_map[256];
#endif
};

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

static int flash_to_fs(int status) {
    return status == FFSV_OK ? FFFS_OK : FFFS_ERR_IO;
}

static int new_flash(struct ffsv_flash **flash, struct fffs_backend *backend) {
    struct ffsv_flash_config cfg;
    int err = ffsv_flash_config_preset(&cfg, FFSV_PRESET_TARGET_NOR_NOTES,
            SWEEP_SECTOR_SIZE * SWEEP_SECTOR_COUNT);
    if (err != FFSV_OK) {
        return FFFS_ERR_IO;
    }
    cfg.sector_size = SWEEP_SECTOR_SIZE;
    cfg.max_log_entries = 200000;
    err = ffsv_flash_create(flash, &cfg);
    if (err != FFSV_OK) {
        return FFFS_ERR_IO;
    }
    return fffs_host_backend_from_verify_flash(backend, *flash);
}

static int mount_storage_init(struct mount_storage *storage) {
    memset(storage, 0, sizeof(*storage));
    storage->index_heads = calloc(SWEEP_INDEX_HEADS,
            sizeof(*storage->index_heads));
    return storage->index_heads ? FFFS_OK : FFFS_ERR_NOMEM;
}

static void mount_storage_destroy(struct mount_storage *storage) {
    free(storage->index_heads);
    *storage = (struct mount_storage){0};
}

static int mount_fs(struct fffs *fs, const struct fffs_backend *backend,
        struct mount_storage *storage) {
    return fffs_mount(fs, backend, &(struct fffs_mount_options){
        .index_heads = storage->index_heads,
        .index_hash_table_size = SWEEP_INDEX_HEADS,
        .scratch = storage->scratch,
        .scratch_size = sizeof(storage->scratch),
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
        .alloc_map = storage->alloc_map,
        .alloc_map_words = sizeof(storage->alloc_map) /
            sizeof(storage->alloc_map[0]),
#endif
    });
}

static void file_name(uint8_t id, char *out, size_t out_size) {
    snprintf(out, out_size, "f%u", (unsigned)id);
}

static int write_file(struct fffs *fs, const char *name,
        const uint8_t *data, size_t size) {
    struct fffs_file file;
    size_t written = 0;
    int err = fffs_open(fs, &file, name,
            FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC);
    if (err != FFFS_OK) {
        return err;
    }
    err = fffs_write(&file, data, size, &written);
    if (err != FFFS_OK) {
        (void)fffs_close(&file);
        return err;
    }
    if (written != size) {
        (void)fffs_close(&file);
        return FFFS_ERR_IO;
    }
    return fffs_close(&file);
}

static int execute_op(struct fffs *fs, const struct sweep_op *op) {
    char name[FFFS_MAX_NAME + 1];
    file_name(op->file_id, name, sizeof(name));
    if (op->type == SWEEP_OP_WRITE) {
        uint8_t value = op->value;
        return write_file(fs, name, &value, sizeof(value));
    }
    int err = fffs_delete_file(fs, name);
    return err == FFFS_ERR_NOT_FOUND ? FFFS_OK : err;
}

static void apply_model(struct sweep_model *model,
        const struct sweep_op *op) {
    if (op->type == SWEEP_OP_WRITE) {
        model->exists[op->file_id] = true;
        model->value[op->file_id] = op->value;
    } else {
        model->exists[op->file_id] = false;
    }
}

static int compare_model(struct fffs *fs, const struct sweep_model *model) {
    size_t expected_count = 0;
    size_t actual_count = 0;
    for (size_t i = 0; i < SWEEP_FILE_COUNT; i++) {
        char name[FFFS_MAX_NAME + 1];
        struct fffs_stat st;
        file_name((uint8_t)i, name, sizeof(name));
        int err = fffs_stat(fs, name, &st);
        if (!model->exists[i]) {
            if (err != FFFS_ERR_NOT_FOUND) {
                return FFFS_ERR_CORRUPT;
            }
            continue;
        }
        expected_count++;
        if (err != FFFS_OK || st.size != 1) {
            return FFFS_ERR_CORRUPT;
        }

        struct fffs_file file;
        uint8_t value = 0;
        size_t nread = 0;
        err = fffs_open(fs, &file, name, FFFS_O_RDONLY);
        if (err != FFFS_OK) {
            return err;
        }
        err = fffs_read(&file, &value, sizeof(value), &nread);
        int close_err = fffs_close(&file);
        if (err != FFFS_OK || close_err != FFFS_OK || nread != 1 ||
                value != model->value[i]) {
            return FFFS_ERR_CORRUPT;
        }
    }

    struct fffs_dir dir;
    struct fffs_stat st;
    int err = fffs_dir_open(fs, &dir, NULL);
    if (err != FFFS_OK) {
        return err;
    }
    while (fffs_dir_read(&dir, &st)) {
        actual_count++;
    }
    err = fffs_dir_status(&dir);
    int close_err = fffs_dir_close(&dir);
    if (err != FFFS_OK || close_err != FFFS_OK) {
        return err != FFFS_OK ? err : close_err;
    }
    return actual_count == expected_count ? FFFS_OK : FFFS_ERR_CORRUPT;
}

static void print_model(const char *label, const struct sweep_model *model) {
    fprintf(stderr, "%s:", label);
    for (size_t i = 0; i < SWEEP_FILE_COUNT; i++) {
        if (model->exists[i]) {
            fprintf(stderr, " f%zu=%u", i, (unsigned)model->value[i]);
        }
    }
    fprintf(stderr, "\n");
}

static int check_invariants(struct fffs_backend *backend, struct fffs *fs,
        const struct sweep_model *before, const struct sweep_model *after) {
    struct fffs_inspect_summary summary;
    int err = fffs_inspect_check(backend, &summary);
    if (err != FFFS_OK) {
        fprintf(stderr, "inspect err=%s\n", fffs_status_name(err));
        return err;
    }
    if (summary.index_corrupt_records || summary.live_entries_corrupt ||
            summary.data_sectors_corrupt || summary.md_corrupt) {
        fprintf(stderr,
                "inspect corrupt index=%zu live=%zu data=%zu md=%zu\n",
                summary.index_corrupt_records,
                summary.live_entries_corrupt,
                summary.data_sectors_corrupt, summary.md_corrupt);
        return FFFS_ERR_CORRUPT;
    }

    err = compare_model(fs, before);
    if (err == FFFS_OK) {
        return FFFS_OK;
    }
    err = compare_model(fs, after);
    if (err != FFFS_OK) {
        print_model("before", before);
        print_model("after", after);
    }
    return err;
}

static int collect_mutations(const struct ffsv_op_record *log,
        size_t first, size_t count, uint64_t *out, size_t *out_count) {
    *out_count = 0;
    for (size_t i = first; i < count; i++) {
        if (log[i].type != FFSV_OP_PROGRAM && log[i].type != FFSV_OP_ERASE) {
            continue;
        }
        if (*out_count >= SWEEP_MAX_MUTATIONS_PER_OP) {
            return FFFS_ERR_RANGE;
        }
        out[(*out_count)++] = log[i].sequence;
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

static int sweep_op_crashes(const struct ffsv_flash_snapshot *snapshot,
        const struct sweep_op *op, const struct sweep_model *before,
        const struct sweep_model *after, struct sweep_stats *stats,
        uint32_t workload_id, size_t op_index) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct mount_storage storage;
    uint64_t mutations[SWEEP_MAX_MUTATIONS_PER_OP];
    size_t mutation_count = 0;

    int err = mount_storage_init(&storage);
    if (err != FFFS_OK) {
        return err;
    }
    err = flash_to_fs(ffsv_flash_reopen_from_snapshot(&flash, snapshot));
    if (err != FFFS_OK) {
        mount_storage_destroy(&storage);
        return err;
    }
    err = fffs_host_backend_from_verify_flash(&backend, flash);
    if (err == FFFS_OK) {
        err = mount_fs(&fs, &backend, &storage);
    }
    size_t log_before = 0;
    if (err == FFFS_OK) {
        (void)ffsv_flash_log(flash, &log_before);
        err = execute_op(&fs, op);
    }
    size_t log_count = 0;
    const struct ffsv_op_record *log = ffsv_flash_log(flash, &log_count);
    if (err == FFFS_OK) {
        err = collect_mutations(log, log_before, log_count, mutations,
                &mutation_count);
    }
    if (err == FFFS_OK) {
        fffs_unmount(&fs);
    }
    ffsv_flash_destroy(flash);
    mount_storage_destroy(&storage);
    if (err != FFFS_OK) {
        return err;
    }

    for (size_t i = 0; i < mutation_count; i++) {
        for (size_t phase_i = 0; phase_i < 2; phase_i++) {
            enum ffsv_failure_phase phase = phase_i == 0 ?
                FFSV_FAIL_BEFORE : FFSV_FAIL_AFTER;
            struct ffsv_flash *attempt_flash = NULL;
            struct fffs_backend attempt_backend;
            struct fffs attempt_fs;
            struct fffs recovered_fs;
            struct mount_storage attempt_storage;
            struct mount_storage recovered_storage;

            err = mount_storage_init(&attempt_storage);
            if (err != FFFS_OK) {
                return err;
            }
            err = mount_storage_init(&recovered_storage);
            if (err != FFFS_OK) {
                mount_storage_destroy(&attempt_storage);
                return err;
            }
            err = flash_to_fs(ffsv_flash_reopen_from_snapshot(&attempt_flash,
                        snapshot));
            if (err != FFFS_OK) {
                mount_storage_destroy(&attempt_storage);
                mount_storage_destroy(&recovered_storage);
                return err;
            }
            err = fffs_host_backend_from_verify_flash(&attempt_backend,
                    attempt_flash);
            if (err == FFFS_OK) {
                err = mount_fs(&attempt_fs, &attempt_backend,
                        &attempt_storage);
            }
            if (err == FFFS_OK) {
                ffsv_flash_set_failure(attempt_flash,
                        &(struct ffsv_failure_injection){
                            .enabled = true,
                            .sequence = mutations[i],
                            .op_mask = (UINT32_C(1) << FFSV_OP_PROGRAM) |
                                (UINT32_C(1) << FFSV_OP_ERASE),
                            .phase = phase,
                            .status = FFSV_ERR_INJECTED,
                        });
                err = execute_op(&attempt_fs, op);
                ffsv_flash_clear_failure(attempt_flash);
                bool injected = log_has_injected_sequence(attempt_flash,
                        mutations[i]);
                fffs_unmount(&attempt_fs);
                if (!injected) {
                    err = FFFS_ERR_CORRUPT;
                } else {
                    err = mount_fs(&recovered_fs, &attempt_backend,
                            &recovered_storage);
                    if (err == FFFS_OK) {
                        err = check_invariants(&attempt_backend,
                                &recovered_fs, before, after);
                        fffs_unmount(&recovered_fs);
                    }
                }
            }
            ffsv_flash_destroy(attempt_flash);
            mount_storage_destroy(&attempt_storage);
            mount_storage_destroy(&recovered_storage);
            stats->crash_points++;
            if (err != FFFS_OK) {
                stats->invariant_failures++;
                fprintf(stderr,
                        "failure workload=%u op=%zu type=%u file=%u "
                        "value=%u seq=%llu phase=%d err=%s\n",
                        (unsigned)workload_id, op_index,
                        (unsigned)op->type, (unsigned)op->file_id,
                        (unsigned)op->value,
                        (unsigned long long)mutations[i], phase,
                        fffs_status_name(err));
                return err;
            }
        }
    }

    return FFFS_OK;
}

static int run_workload(const struct sweep_op *ops, size_t op_count,
        struct sweep_stats *stats, uint32_t workload_id) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct mount_storage storage;
    struct sweep_model model = {0};

    int err = mount_storage_init(&storage);
    if (err != FFFS_OK) {
        return err;
    }
    err = new_flash(&flash, &backend);
    if (err != FFFS_OK) {
        mount_storage_destroy(&storage);
        return err;
    }
    err = fffs_format(&backend, &(struct fffs_format_options){
        .index_sectors = SWEEP_INDEX_SECTORS,
        .sector_size = FFFS_SECTOR_256,
    });
    if (err == FFFS_OK) {
        err = mount_fs(&fs, &backend, &storage);
    }

    for (size_t i = 0; err == FFFS_OK && i < op_count; i++) {
        struct ffsv_flash_snapshot before_snapshot;
        struct sweep_model before_model = model;
        struct sweep_model after_model = model;
        apply_model(&after_model, &ops[i]);

        err = flash_to_fs(ffsv_flash_snapshot_create(flash,
                    &before_snapshot));
        if (err != FFFS_OK) {
            break;
        }

        err = sweep_op_crashes(&before_snapshot, &ops[i], &before_model,
                &after_model, stats, workload_id, i);
        ffsv_flash_snapshot_destroy(&before_snapshot);
        if (err != FFFS_OK) {
            break;
        }

        err = execute_op(&fs, &ops[i]);
        if (err == FFFS_OK) {
            model = after_model;
            err = check_invariants(&backend, &fs, &model, &model);
        }
    }

    if (err == FFFS_OK) {
        fffs_unmount(&fs);
    }
    ffsv_flash_destroy(flash);
    mount_storage_destroy(&storage);
    return err;
}

static uint32_t rng_next(uint32_t *rng) {
    *rng = *rng * UINT32_C(1664525) + UINT32_C(1013904223);
    return *rng;
}

static void random_workload(uint32_t seed, struct sweep_op *ops,
        size_t op_count) {
    uint32_t rng = seed;
    for (size_t i = 0; i < op_count; i++) {
        uint32_t r = rng_next(&rng);
        ops[i] = (struct sweep_op){
            .type = (r & 3u) == 0u ? SWEEP_OP_DELETE : SWEEP_OP_WRITE,
            .file_id = (uint8_t)((r >> 8) % SWEEP_FILE_COUNT),
            .value = (uint8_t)(r >> 16),
        };
    }
}

static int run_random(size_t workloads, size_t ops_per_workload,
        struct sweep_stats *stats) {
    struct sweep_op ops[SWEEP_MAX_OPS];
    if (ops_per_workload > SWEEP_MAX_OPS) {
        return FFFS_ERR_INVALID;
    }
    for (size_t i = 0; i < workloads; i++) {
        random_workload((uint32_t)(0xc001d00du + i * 97u), ops,
                ops_per_workload);
        int err = run_workload(ops, ops_per_workload, stats, (uint32_t)i);
        stats->random_workloads++;
        if (err != FFFS_OK) {
            return err;
        }
    }
    return FFFS_OK;
}

static int run_tiny_exhaustive_rec(struct sweep_op *ops, size_t depth,
        size_t max_depth, struct sweep_stats *stats) {
    if (depth == max_depth) {
        int err = run_workload(ops, max_depth, stats,
                (uint32_t)(UINT32_C(0x80000000) | stats->tiny_cases));
        stats->tiny_cases++;
        return err;
    }

    for (size_t file = 0; file < 3; file++) {
        ops[depth] = (struct sweep_op){
            .type = SWEEP_OP_WRITE,
            .file_id = (uint8_t)file,
            .value = (uint8_t)(depth * 17u + file),
        };
        int err = run_tiny_exhaustive_rec(ops, depth + 1, max_depth, stats);
        if (err != FFFS_OK) {
            return err;
        }

        ops[depth] = (struct sweep_op){
            .type = SWEEP_OP_DELETE,
            .file_id = (uint8_t)file,
            .value = 0,
        };
        err = run_tiny_exhaustive_rec(ops, depth + 1, max_depth, stats);
        if (err != FFFS_OK) {
            return err;
        }
    }
    return FFFS_OK;
}

static int run_tiny_exhaustive(size_t depth, struct sweep_stats *stats) {
    struct sweep_op ops[SWEEP_MAX_OPS];
    if (depth > SWEEP_MAX_OPS) {
        return FFFS_ERR_INVALID;
    }
    return run_tiny_exhaustive_rec(ops, 0, depth, stats);
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

int main(int argc, char **argv) {
    size_t random_workloads = parse_arg(argc > 1 ? argv[1] : NULL, 10);
    size_t ops_per_workload = parse_arg(argc > 2 ? argv[2] : NULL, 24);
    size_t tiny_depth = parse_arg(argc > 3 ? argv[3] : NULL, 3);
    struct sweep_stats stats = {0};

    printf("fffs crash sweep [%s]\n", cache_mode_name());
    int err = run_random(random_workloads, ops_per_workload, &stats);
    if (err == FFFS_OK && tiny_depth > 0) {
        err = run_tiny_exhaustive(tiny_depth, &stats);
    }

    print_count_line("crash points tested", stats.crash_points);
    print_count_line("random workloads", stats.random_workloads);
    print_count_line("tiny-volume exhaustive cases", stats.tiny_cases);
    print_count_line("invariant failures", stats.invariant_failures);
    if (err != FFFS_OK) {
        fprintf(stderr, "crash sweep failed: %s\n", fffs_status_name(err));
        return 1;
    }
    return 0;
}
