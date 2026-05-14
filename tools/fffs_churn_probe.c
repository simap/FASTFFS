#include "fastffs/fastffs.h"
#include "fastffs/fastffs_host.h"
#include "fastffs/verify_flash.h"
#include "churn_model.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
#define INDEX_HASH_TABLE_SIZE FFFS_SLOT_COUNT
#else
#define INDEX_HASH_TABLE_SIZE FFFS_INDEX_HASH_TABLE_SIZE
#endif

#ifndef FFFS_HOST_CHURN_FLASH_SIZE
#define FFFS_HOST_CHURN_FLASH_SIZE (2u * 1024u * 1024u)
#endif

#ifndef FFFS_HOST_CHURN_TARGET_LIVE_BYTES
#define FFFS_HOST_CHURN_TARGET_LIVE_BYTES (512u * 1024u)
#endif

#ifndef FFFS_HOST_CHURN_TARGET_WRITTEN_BYTES
#define FFFS_HOST_CHURN_TARGET_WRITTEN_BYTES (1u * 1024u * 1024u)
#endif

#ifndef FFFS_HOST_CHURN_TARGET_SLACK_BYTES
#define FFFS_HOST_CHURN_TARGET_SLACK_BYTES (64u * 1024u)
#endif

#ifndef FFFS_HOST_CHURN_FORCE_LARGE_AFTER_BYTES
#define FFFS_HOST_CHURN_FORCE_LARGE_AFTER_BYTES (768u * 1024u)
#endif

#ifndef FFFS_HOST_CHURN_GC_STEPS_PER_OP
#define FFFS_HOST_CHURN_GC_STEPS_PER_OP 16u
#endif

#ifndef FFFS_HOST_CHURN_FORCED_GC_STEPS
#define FFFS_HOST_CHURN_FORCED_GC_STEPS 1024u
#endif

#ifndef FFFS_HOST_CHURN_SCRATCH_SIZE
#define FFFS_HOST_CHURN_SCRATCH_SIZE 4096u
#endif

#define FFFS_HOST_CHURN_SEED 0x4f465346u
#define MAX_CHURN_FILE_SIZE (350u * 1024u)

#ifndef FFFS_HOST_CHURN_IMAGE_PREFIX
#define FFFS_HOST_CHURN_IMAGE_PREFIX "churn"
#endif

#if FFFS_HOST_CHURN_SCRATCH_SIZE < FFFS_MIN_SCRATCH_SIZE
#error "FFFS_HOST_CHURN_SCRATCH_SIZE must be at least FFFS_MIN_SCRATCH_SIZE"
#endif

struct op_summary {
    uint64_t calls;
    uint64_t bytes;
    uint64_t ns;
};

struct class_stats {
    uint64_t ops;
    uint64_t bytes;
    uint64_t ns;
    uint64_t flash_reads;
    uint64_t flash_programs;
    uint64_t flash_erases;
    uint64_t flash_blank_checks;
};

static uint16_t index_heads[INDEX_HASH_TABLE_SIZE];
static uint16_t remount_heads[INDEX_HASH_TABLE_SIZE];
static uint8_t scratch[FFFS_HOST_CHURN_SCRATCH_SIZE];
static uint8_t *io_buffer;

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

static int mount_fs(struct fffs *fs, const struct fffs_backend *backend,
        uint16_t *heads) {
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    static uint32_t alloc_map[2048];
#endif
    memset(heads, 0, sizeof(uint16_t) * INDEX_HASH_TABLE_SIZE);
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    memset(alloc_map, 0, sizeof(alloc_map));
#endif
    return fffs_mount(fs, backend, &(struct fffs_mount_options){
        .index_heads = heads,
        .index_hash_table_size = INDEX_HASH_TABLE_SIZE,
        .scratch = scratch,
        .scratch_size = sizeof(scratch),
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
        .alloc_map = alloc_map,
        .alloc_map_words = sizeof(alloc_map) / sizeof(alloc_map[0]),
#endif
    });
}

static void fill_pattern(uint8_t *dst, size_t size, uint32_t seed) {
    for (size_t i = 0; i < size; ++i) {
        dst[i] = (uint8_t)(seed + i * 33u + (i >> 3));
    }
}

static int write_file(struct fffs *fs, const char *name, size_t size,
        uint32_t seed) {
    struct fffs_file file;
    size_t written = 0;
    fill_pattern(io_buffer, size, seed);

    int err = fffs_open(fs, &file, name,
            FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC);
    if (err != FFFS_OK) {
        return err;
    }
    err = fffs_write(&file, io_buffer, size, &written);
    if (err == FFFS_OK && written != size) {
        err = FFFS_ERR_IO;
    }
    int close_err = fffs_close(&file);
    if (err != FFFS_OK) {
        return err;
    }
    return close_err;
}

static int read_file(struct fffs *fs, const char *name, size_t expected_size,
        uint32_t seed) {
    struct fffs_file file;
    size_t total = 0;
    int err = fffs_open(fs, &file, name, FFFS_O_RDONLY);
    if (err != FFFS_OK) {
        return err;
    }
    while (total < expected_size) {
        size_t chunk = expected_size - total;
        size_t nread = 0;
        if (chunk > 1024u) {
            chunk = 1024u;
        }
        err = fffs_read(&file, io_buffer + total, chunk, &nread);
        if (err != FFFS_OK) {
            (void)fffs_close(&file);
            return err;
        }
        if (nread == 0) {
            (void)fffs_close(&file);
            return FFFS_ERR_IO;
        }
        total += nread;
    }
    err = fffs_close(&file);
    if (err != FFFS_OK) {
        return err;
    }
    for (size_t i = 0; i < expected_size; ++i) {
        uint8_t want = (uint8_t)(seed + i * 33u + (i >> 3));
        if (io_buffer[i] != want) {
            return FFFS_ERR_CORRUPT;
        }
    }
    return FFFS_OK;
}

static int list_all(struct fffs *fs, size_t *out_count) {
    struct fffs_dir dir;
    struct fffs_stat st;
    size_t count = 0;
    int err = fffs_dir_open(fs, &dir, "");
    if (err != FFFS_OK) {
        return err;
    }
    while (fffs_dir_read(&dir, &st)) {
        count++;
    }
    err = fffs_dir_status(&dir);
    int close_err = fffs_dir_close(&dir);
    if (err != FFFS_OK) {
        return err;
    }
    if (close_err != FFFS_OK) {
        return close_err;
    }
    *out_count = count;
    return FFFS_OK;
}

static int validate_live_set(struct fffs *fs, const bench_churn_model_t *model,
        const char *label) {
    for (int i = 0; i < BENCH_CHURN_MAX_FILES; ++i) {
        if (!model->slots[i].live) {
            continue;
        }
        bool exists = false;
        int err = fffs_exists(fs, model->slots[i].name, &exists);
        if (err != FFFS_OK) {
            fprintf(stderr, "%s exists %s slot=%d size=%u failed: %s\n",
                    label, model->slots[i].name, i, model->slots[i].size,
                    fffs_status_name(err));
            return err;
        }
        if (!exists) {
            fprintf(stderr, "%s missing live model file name=%s slot=%d "
                    "size=%u class=%s\n",
                    label, model->slots[i].name, i, model->slots[i].size,
                    bench_churn_class_name(model->slots[i].cls));
            return FFFS_ERR_NOT_FOUND;
        }
    }
    return FFFS_OK;
}

static void summarize_ops(struct ffsv_flash *flash, uint64_t before_seq,
        uint64_t after_seq, struct op_summary out[FFSV_OP_COUNT]) {
    memset(out, 0, sizeof(out[0]) * FFSV_OP_COUNT);
    size_t count = 0;
    const struct ffsv_op_record *log = ffsv_flash_log(flash, &count);
    for (size_t i = 0; i < count; i++) {
        const struct ffsv_op_record *r = &log[i];
        if (r->sequence < before_seq || r->sequence >= after_seq ||
                r->type >= FFSV_OP_COUNT) {
            continue;
        }
        out[r->type].calls += 1;
        out[r->type].bytes += r->committed_bytes != 0 ?
            r->committed_bytes : r->size;
        out[r->type].ns += r->time_after_ns - r->time_before_ns;
    }
}

static void add_flash_counts(struct class_stats *stats,
        const struct op_summary ops[FFSV_OP_COUNT]) {
    stats->flash_reads += ops[FFSV_OP_READ].calls;
    stats->flash_programs += ops[FFSV_OP_PROGRAM].calls;
    stats->flash_erases += ops[FFSV_OP_ERASE].calls;
    stats->flash_blank_checks += ops[FFSV_OP_BLANK_CHECK].calls;
}

static const char *class_name(bench_churn_class_t cls) {
    return bench_churn_class_name(cls);
}

static void print_class_stats(const char *label,
        const struct class_stats stats[BENCH_CHURN_CLASS_COUNT]) {
    for (int i = 0; i < BENCH_CHURN_CLASS_COUNT; ++i) {
        double ms = (double)stats[i].ns / 1000000.0;
        double kib_s = stats[i].ns == 0 ? 0.0 :
            ((double)stats[i].bytes * 1000000000.0) /
            ((double)stats[i].ns * 1024.0);
        printf("%-13s %-14s ops=%4llu bytes=%8llu time=%9.3f ms "
               "throughput=%8.1f KiB/s flash_ops=r%llu/p%llu/e%llu/bc%llu\n",
               label, class_name((bench_churn_class_t)i),
               (unsigned long long)stats[i].ops,
               (unsigned long long)stats[i].bytes,
               ms, kib_s,
               (unsigned long long)stats[i].flash_reads,
               (unsigned long long)stats[i].flash_programs,
               (unsigned long long)stats[i].flash_erases,
               (unsigned long long)stats[i].flash_blank_checks);
    }
}

static uint32_t count_live_slots(const bench_churn_model_t *model) {
    uint32_t live = 0;
    for (int i = 0; i < BENCH_CHURN_MAX_FILES; ++i) {
        if (model->slots[i].live) {
            live++;
        }
    }
    return live;
}

static int run_gc_steps(struct fffs *fs, size_t steps, uint64_t *out_ns,
        size_t *out_erased) {
    uint64_t before = 0;
    uint64_t after = 0;
    size_t erased = 0;
    if (steps == 0) {
        if (out_ns) {
            *out_ns = 0;
        }
        if (out_erased) {
            *out_erased = 0;
        }
        return FFFS_OK;
    }
    before = ffsv_flash_time_ns(fs->backend.ctx);
    int err = fffs_gc(fs, steps, &erased);
    after = ffsv_flash_time_ns(fs->backend.ctx);
    if (out_ns) {
        *out_ns = after - before;
    }
    if (out_erased) {
        *out_erased = erased;
    }
    return err;
}

static void dump_final_image(struct ffsv_flash *flash, const char *profile_name) {
    char path[256];
    snprintf(path, sizeof(path), "%s-%s.img", FFFS_HOST_CHURN_IMAGE_PREFIX,
            profile_name);
    int err = ffsv_flash_dump_image(flash, path);
    if (err == FFSV_OK) {
        printf("final image             path=%s\n", path);
    } else {
        fprintf(stderr, "final image dump failed path=%s rc=%s\n", path,
                ffsv_status_name(err));
    }
}

static int run_churn(enum ffsv_flash_preset preset, const char *profile_name) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    bench_churn_model_t model;
    struct class_stats write_stats[BENCH_CHURN_CLASS_COUNT] = {0};
    struct class_stats delete_stats[BENCH_CHURN_CLASS_COUNT] = {0};
    struct class_stats read_stats[BENCH_CHURN_CLASS_COUNT] = {0};
    uint64_t gc_ns = 0;
    size_t gc_erased = 0;
    uint64_t forced_gc_ns = 0;
    size_t forced_gc_erased = 0;
    uint32_t write_ops = 0;
    uint32_t delete_ops = 0;
    uint32_t no_space_retries = 0;
    uint32_t slot_seed[BENCH_CHURN_MAX_FILES] = {0};
    int err;

    struct ffsv_flash_config cfg;
    err = ffsv_flash_config_preset(&cfg, preset, FFFS_HOST_CHURN_FLASH_SIZE);
    if (err != FFSV_OK) {
        fprintf(stderr, "flash config failed: %s\n", ffsv_status_name(err));
        return 1;
    }
    cfg.max_log_entries = 262144;
    err = ffsv_flash_create(&flash, &cfg);
    if (err != FFSV_OK) {
        fprintf(stderr, "flash create failed: %s\n", ffsv_status_name(err));
        return 1;
    }
    err = fffs_host_backend_from_verify_flash(&backend, flash);
    if (err != FFFS_OK) {
        fprintf(stderr, "backend create failed: %s\n", fffs_status_name(err));
        dump_final_image(flash, profile_name);
        ffsv_flash_destroy(flash);
        return 1;
    }

    printf("%s churn [%s, flash=%u KiB, target_written=%u KiB]\n",
           profile_name, cache_mode_name(),
           FFFS_HOST_CHURN_FLASH_SIZE / 1024u,
           FFFS_HOST_CHURN_TARGET_WRITTEN_BYTES / 1024u);

    uint64_t format_before = ffsv_flash_time_ns(flash);
    err = fffs_format(&backend, NULL);
    uint64_t format_after = ffsv_flash_time_ns(flash);
    if (err != FFFS_OK) {
        fprintf(stderr, "format failed: %s\n", fffs_status_name(err));
        dump_final_image(flash, profile_name);
        ffsv_flash_destroy(flash);
        return 1;
    }
    err = mount_fs(&fs, &backend, index_heads);
    if (err != FFFS_OK) {
        fprintf(stderr, "mount failed: %s\n", fffs_status_name(err));
        dump_final_image(flash, profile_name);
        ffsv_flash_destroy(flash);
        return 1;
    }

    bench_churn_model_init(&model, FFFS_HOST_CHURN_SEED,
            FFFS_HOST_CHURN_TARGET_LIVE_BYTES,
            FFFS_HOST_CHURN_TARGET_WRITTEN_BYTES,
            FFFS_HOST_CHURN_TARGET_SLACK_BYTES,
            FFFS_HOST_CHURN_FORCE_LARGE_AFTER_BYTES);

    uint64_t churn_before_seq = ffsv_flash_next_sequence(flash);
    uint64_t churn_before = ffsv_flash_time_ns(flash);
    while (true) {
        bench_churn_event_t event;
        bench_churn_event_type_t type = bench_churn_model_next(&model, &event);
        if (type == BENCH_CHURN_EVENT_DONE) {
            break;
        }
        if (type == BENCH_CHURN_EVENT_NO_SLOT) {
            fprintf(stderr, "churn model ran out of slots\n");
            err = FFFS_ERR_NO_SPACE;
            break;
        }

        uint64_t before_seq = ffsv_flash_next_sequence(flash);
        uint64_t before = ffsv_flash_time_ns(flash);
        if (type == BENCH_CHURN_EVENT_DELETE) {
            err = fffs_delete_file(&fs, event.name);
            uint64_t after = ffsv_flash_time_ns(flash);
            uint64_t after_seq = ffsv_flash_next_sequence(flash);
            if (err != FFFS_OK) {
                fprintf(stderr, "delete %s failed: %s\n", event.name,
                        fffs_status_name(err));
                break;
            }
            struct op_summary ops[FFSV_OP_COUNT];
            summarize_ops(flash, before_seq, after_seq, ops);
            struct class_stats *s = &delete_stats[event.cls];
            s->ops++;
            s->bytes += event.size;
            s->ns += after - before;
            add_flash_counts(s, ops);
            bench_churn_model_apply(&model, &event);
            delete_ops++;
        } else {
            err = write_file(&fs, event.name, event.size, event.write_seed);
            if (err == FFFS_ERR_NO_SPACE) {
                no_space_retries++;
                uint64_t local_gc_ns = 0;
                size_t local_erased = 0;
                int gc_err = run_gc_steps(&fs, FFFS_HOST_CHURN_FORCED_GC_STEPS,
                        &local_gc_ns, &local_erased);
                forced_gc_ns += local_gc_ns;
                forced_gc_erased += local_erased;
                if (gc_err == FFFS_OK) {
                    before_seq = ffsv_flash_next_sequence(flash);
                    before = ffsv_flash_time_ns(flash);
                    err = write_file(&fs, event.name, event.size,
                            event.write_seed);
                }
            }
            uint64_t after = ffsv_flash_time_ns(flash);
            uint64_t after_seq = ffsv_flash_next_sequence(flash);
            if (err != FFFS_OK) {
                fprintf(stderr, "write %s size=%u failed: %s\n", event.name,
                        event.size, fffs_status_name(err));
                break;
            }
            struct op_summary ops[FFSV_OP_COUNT];
            summarize_ops(flash, before_seq, after_seq, ops);
            struct class_stats *s = &write_stats[event.cls];
            s->ops++;
            s->bytes += event.size;
            s->ns += after - before;
            add_flash_counts(s, ops);
            bench_churn_model_apply(&model, &event);
            slot_seed[event.slot] = event.write_seed;
            write_ops++;
        }

        uint64_t local_gc_ns = 0;
        size_t local_erased = 0;
        int gc_err = run_gc_steps(&fs, FFFS_HOST_CHURN_GC_STEPS_PER_OP,
                &local_gc_ns, &local_erased);
        gc_ns += local_gc_ns;
        gc_erased += local_erased;
        if (gc_err != FFFS_OK) {
            fprintf(stderr, "gc failed: %s\n", fffs_status_name(gc_err));
            err = gc_err;
            break;
        }
    }
    uint64_t churn_after = ffsv_flash_time_ns(flash);
    uint64_t churn_after_seq = ffsv_flash_next_sequence(flash);

    if (err != FFFS_OK) {
        fffs_unmount(&fs);
        dump_final_image(flash, profile_name);
        ffsv_flash_destroy(flash);
        return 1;
    }

    size_t listed = 0;
    uint64_t list_before_seq = ffsv_flash_next_sequence(flash);
    uint64_t list_before = ffsv_flash_time_ns(flash);
    err = list_all(&fs, &listed);
    uint64_t list_after = ffsv_flash_time_ns(flash);
    uint64_t list_after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        fprintf(stderr, "list failed: %s\n", fffs_status_name(err));
        fffs_unmount(&fs);
        dump_final_image(flash, profile_name);
        ffsv_flash_destroy(flash);
        return 1;
    }
    err = validate_live_set(&fs, &model, "warm");
    if (err != FFFS_OK) {
        fffs_unmount(&fs);
        dump_final_image(flash, profile_name);
        ffsv_flash_destroy(flash);
        return 1;
    }
    fffs_unmount(&fs);

    uint64_t mount_before_seq = ffsv_flash_next_sequence(flash);
    uint64_t mount_before = ffsv_flash_time_ns(flash);
    err = mount_fs(&remounted, &backend, remount_heads);
    uint64_t mount_after = ffsv_flash_time_ns(flash);
    uint64_t mount_after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        fprintf(stderr, "cold mount failed: %s\n", fffs_status_name(err));
        dump_final_image(flash, profile_name);
        ffsv_flash_destroy(flash);
        return 1;
    }
    err = validate_live_set(&remounted, &model, "cold");
    if (err != FFFS_OK) {
        fffs_unmount(&remounted);
        dump_final_image(flash, profile_name);
        ffsv_flash_destroy(flash);
        return 1;
    }

    uint64_t read_before_seq = ffsv_flash_next_sequence(flash);
    uint64_t read_before = ffsv_flash_time_ns(flash);
    size_t read_files = 0;
    for (int pass = 0; pass < BENCH_CHURN_MAX_FILES; ++pass) {
        int slot = (pass * 73 + 19) % BENCH_CHURN_MAX_FILES;
        if (!model.slots[slot].live) {
            continue;
        }
        bench_churn_slot_t *s = &model.slots[slot];
        uint64_t before_seq = ffsv_flash_next_sequence(flash);
        uint64_t before = ffsv_flash_time_ns(flash);
        err = read_file(&remounted, s->name, s->size, slot_seed[slot]);
        uint64_t after = ffsv_flash_time_ns(flash);
        uint64_t after_seq = ffsv_flash_next_sequence(flash);
        if (err != FFFS_OK) {
            fprintf(stderr, "read %s failed: %s\n", s->name,
                    fffs_status_name(err));
            fffs_unmount(&remounted);
            dump_final_image(flash, profile_name);
            ffsv_flash_destroy(flash);
            return 1;
        }
        struct op_summary ops[FFSV_OP_COUNT];
        summarize_ops(flash, before_seq, after_seq, ops);
        struct class_stats *rs = &read_stats[s->cls];
        rs->ops++;
        rs->bytes += s->size;
        rs->ns += after - before;
        add_flash_counts(rs, ops);
        read_files++;
    }
    uint64_t read_after = ffsv_flash_time_ns(flash);
    uint64_t read_after_seq = ffsv_flash_next_sequence(flash);

    struct op_summary churn_ops[FFSV_OP_COUNT];
    struct op_summary list_ops[FFSV_OP_COUNT];
    struct op_summary mount_ops[FFSV_OP_COUNT];
    struct op_summary read_ops[FFSV_OP_COUNT];
    summarize_ops(flash, churn_before_seq, churn_after_seq, churn_ops);
    summarize_ops(flash, list_before_seq, list_after_seq, list_ops);
    summarize_ops(flash, mount_before_seq, mount_after_seq, mount_ops);
    summarize_ops(flash, read_before_seq, read_after_seq, read_ops);

    printf("format                  time=%9.3f ms\n",
           (double)(format_after - format_before) / 1000000.0);
    printf("churn summary           writes=%u deletes=%u creates=%u replaces=%u "
           "live_files=%u live_bytes=%u total_written=%u no_space_retries=%u\n",
           write_ops, delete_ops, model.create_count, model.replace_count,
           count_live_slots(&model), model.live_bytes, model.total_written,
           no_space_retries);
    printf("churn total             time=%9.3f ms flash_ops=r%llu/p%llu/e%llu/bc%llu\n",
           (double)(churn_after - churn_before) / 1000000.0,
           (unsigned long long)churn_ops[FFSV_OP_READ].calls,
           (unsigned long long)churn_ops[FFSV_OP_PROGRAM].calls,
           (unsigned long long)churn_ops[FFSV_OP_ERASE].calls,
           (unsigned long long)churn_ops[FFSV_OP_BLANK_CHECK].calls);
    printf("gc idle                 steps_per_op=%u time=%9.3f ms erased=%zu\n",
           FFFS_HOST_CHURN_GC_STEPS_PER_OP,
           (double)gc_ns / 1000000.0, gc_erased);
    printf("gc forced               steps=%u time=%9.3f ms erased=%zu\n",
           FFFS_HOST_CHURN_FORCED_GC_STEPS,
           (double)forced_gc_ns / 1000000.0, forced_gc_erased);
    print_class_stats("write", write_stats);
    print_class_stats("delete", delete_stats);
    printf("warm list               files=%zu time=%9.3f ms flash_ops=r%llu/p%llu/e%llu/bc%llu\n",
           listed, (double)(list_after - list_before) / 1000000.0,
           (unsigned long long)list_ops[FFSV_OP_READ].calls,
           (unsigned long long)list_ops[FFSV_OP_PROGRAM].calls,
           (unsigned long long)list_ops[FFSV_OP_ERASE].calls,
           (unsigned long long)list_ops[FFSV_OP_BLANK_CHECK].calls);
    printf("cold mount              time=%9.3f ms flash_ops=r%llu/p%llu/e%llu/bc%llu\n",
           (double)(mount_after - mount_before) / 1000000.0,
           (unsigned long long)mount_ops[FFSV_OP_READ].calls,
           (unsigned long long)mount_ops[FFSV_OP_PROGRAM].calls,
           (unsigned long long)mount_ops[FFSV_OP_ERASE].calls,
           (unsigned long long)mount_ops[FFSV_OP_BLANK_CHECK].calls);
    printf("cold read summary       files=%zu time=%9.3f ms flash_ops=r%llu/p%llu/e%llu/bc%llu\n",
           read_files, (double)(read_after - read_before) / 1000000.0,
           (unsigned long long)read_ops[FFSV_OP_READ].calls,
           (unsigned long long)read_ops[FFSV_OP_PROGRAM].calls,
           (unsigned long long)read_ops[FFSV_OP_ERASE].calls,
           (unsigned long long)read_ops[FFSV_OP_BLANK_CHECK].calls);
    print_class_stats("cold read", read_stats);

    fffs_unmount(&remounted);
    dump_final_image(flash, profile_name);
    ffsv_flash_destroy(flash);
    return 0;
}

int main(void) {
    io_buffer = malloc(MAX_CHURN_FILE_SIZE);
    if (!io_buffer) {
        return 1;
    }
    int err = run_churn(FFSV_PRESET_TARGET_NOR_NOTES, "target-nor-notes");
    if (err == 0) {
        printf("\n");
        err = run_churn(FFSV_PRESET_ESP32S3_MEASURED, "esp32s3-measured");
    }
    free(io_buffer);
    return err;
}
