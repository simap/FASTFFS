#include "fastffs/fastffs.h"
#include "fastffs/fastffs_host.h"
#include "fastffs/verify_flash.h"
#include "churn_model.h"
#include "../src/fffs_internal.h"

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
#define INDEX_CACHE_WORDS \
    (((FFFS_INDEX_CACHE_BYTES(INDEX_HASH_TABLE_SIZE) + sizeof(uint32_t) - 1u) / \
      sizeof(uint32_t)) ? \
     ((FFFS_INDEX_CACHE_BYTES(INDEX_HASH_TABLE_SIZE) + sizeof(uint32_t) - 1u) / \
      sizeof(uint32_t)) : 1u)

#ifndef FFFS_HOST_CHURN_PROFILE_SMALL_FILES
#define FFFS_HOST_CHURN_PROFILE_SMALL_FILES 0
#endif

#ifndef FFFS_HOST_CHURN_FLASH_SIZE
#if FFFS_HOST_CHURN_PROFILE_SMALL_FILES
#define FFFS_HOST_CHURN_FLASH_SIZE (4u * 1024u * 1024u)
#else
#define FFFS_HOST_CHURN_FLASH_SIZE (2u * 1024u * 1024u)
#endif
#endif

#ifndef FFFS_HOST_CHURN_TARGET_LIVE_BYTES
#if FFFS_HOST_CHURN_PROFILE_SMALL_FILES
#define FFFS_HOST_CHURN_TARGET_LIVE_BYTES 2308848u
#else
#define FFFS_HOST_CHURN_TARGET_LIVE_BYTES (512u * 1024u)
#endif
#endif

#ifndef FFFS_HOST_CHURN_TARGET_WRITTEN_BYTES
#if FFFS_HOST_CHURN_PROFILE_SMALL_FILES
#define FFFS_HOST_CHURN_TARGET_WRITTEN_BYTES (8u * 1024u * 1024u)
#else
#define FFFS_HOST_CHURN_TARGET_WRITTEN_BYTES (1u * 1024u * 1024u)
#endif
#endif

#ifndef FFFS_HOST_CHURN_TARGET_SLACK_BYTES
#if FFFS_HOST_CHURN_PROFILE_SMALL_FILES
#define FFFS_HOST_CHURN_TARGET_SLACK_BYTES (128u * 1024u)
#else
#define FFFS_HOST_CHURN_TARGET_SLACK_BYTES (64u * 1024u)
#endif
#endif

#ifndef FFFS_HOST_CHURN_FORCE_LARGE_AFTER_BYTES
#if FFFS_HOST_CHURN_PROFILE_SMALL_FILES
#define FFFS_HOST_CHURN_FORCE_LARGE_AFTER_BYTES UINT32_MAX
#else
#define FFFS_HOST_CHURN_FORCE_LARGE_AFTER_BYTES (768u * 1024u)
#endif
#endif

#ifndef FFFS_HOST_CHURN_MAX_FILES
#if FFFS_HOST_CHURN_PROFILE_SMALL_FILES
#define FFFS_HOST_CHURN_MAX_FILES 5000u
#else
#define FFFS_HOST_CHURN_MAX_FILES BENCH_CHURN_MAX_FILES
#endif
#endif

#ifndef FFFS_HOST_CHURN_SMALL_MIN_SIZE
#define FFFS_HOST_CHURN_SMALL_MIN_SIZE 1u
#endif

#ifndef FFFS_HOST_CHURN_SMALL_MAX_SIZE
#define FFFS_HOST_CHURN_SMALL_MAX_SIZE (5u * 1024u)
#endif

#ifndef FFFS_HOST_CHURN_GC_STEPS_PER_OP
#define FFFS_HOST_CHURN_GC_STEPS_PER_OP 16u
#endif

#ifndef FFFS_HOST_CHURN_SCRATCH_SIZE
#define FFFS_HOST_CHURN_SCRATCH_SIZE 4096u
#endif

#define FFFS_HOST_CHURN_SEED 0x4f465346u
#if FFFS_HOST_CHURN_PROFILE_SMALL_FILES
#define MAX_CHURN_FILE_SIZE FFFS_HOST_CHURN_SMALL_MAX_SIZE
#else
#define MAX_CHURN_FILE_SIZE (350u * 1024u)
#endif

#ifndef FFFS_HOST_CHURN_IMAGE_PREFIX
#define FFFS_HOST_CHURN_IMAGE_PREFIX "churn"
#endif

#if FFFS_HOST_CHURN_SCRATCH_SIZE < FFFS_MIN_SCRATCH_SIZE
#error "FFFS_HOST_CHURN_SCRATCH_SIZE must be at least FFFS_MIN_SCRATCH_SIZE"
#endif

static const bench_churn_profile_t *host_churn_profile(void);

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

enum profile_bucket {
    PROFILE_INDEX_READ,
    PROFILE_INDEX_PROGRAM,
    PROFILE_MD_TAIL_READ,
    PROFILE_MD_TAIL_PROGRAM,
    PROFILE_FOOTER_READ,
    PROFILE_FOOTER_PROGRAM,
    PROFILE_FOOTER_TOMBSTONE,
    PROFILE_SECTOR_SCAN_READ,
    PROFILE_DATA_READ,
    PROFILE_DATA_PROGRAM,
    PROFILE_ERASE,
    PROFILE_OTHER,
    PROFILE_COUNT,
};

struct profile_stats {
    uint64_t calls;
    uint64_t bytes;
    uint64_t ns;
};

static uint32_t index_cache[INDEX_CACHE_WORDS];
static uint32_t remount_cache[INDEX_CACHE_WORDS];
static uint8_t scratch[FFFS_HOST_CHURN_SCRATCH_SIZE];
static bench_churn_slot_t model_slots[FFFS_HOST_CHURN_MAX_FILES];
static uint32_t slot_seed[FFFS_HOST_CHURN_MAX_FILES];
static uint8_t *io_buffer;

#if FFFS_PROFILE_TRACE
struct scope_profile {
    uint64_t calls;
    uint64_t bytes;
    uint64_t ns;
    uint64_t reads;
    uint64_t programs;
    uint64_t erases;
};

struct trace_capture {
    struct ffsv_flash *flash;
    uint64_t last_ns;
    struct scope_profile scopes[FFFS_PROFILE_COUNT];
};

static struct trace_capture *active_trace;
static struct trace_capture *active_extra_trace;

static const char *scope_name(enum fffs_profile_scope scope) {
    switch (scope) {
    case FFFS_PROFILE_UNSCOPED:
        return "unscoped";
    case FFFS_PROFILE_MOUNT:
        return "mount";
    case FFFS_PROFILE_INDEX_REPLAY:
        return "index replay";
    case FFFS_PROFILE_INDEX_RESOLVE:
        return "index resolve";
    case FFFS_PROFILE_DIR_READ:
        return "dir read";
    case FFFS_PROFILE_READ_METADATA:
        return "read metadata";
    case FFFS_PROFILE_SPAN_IS_ERASED:
        return "span is erased";
    case FFFS_PROFILE_ALLOC_NEXT_SECTOR:
        return "alloc next sector";
    case FFFS_PROFILE_GC:
        return "gc";
    case FFFS_PROFILE_GC_STEP:
        return "gc step";
    case FFFS_PROFILE_GC_FOOTER_STATE:
        return "gc footer state";
    case FFFS_PROFILE_GC_REACHABILITY:
        return "gc reachability";
    case FFFS_PROFILE_READ:
        return "read";
    case FFFS_PROFILE_WRITE:
        return "write";
    case FFFS_PROFILE_CLOSE:
        return "close";
    case FFFS_PROFILE_DELETE:
        return "delete";
    default:
        return "unknown";
    }
}

static void trace_reset(struct trace_capture *trace,
        struct ffsv_flash *flash) {
    memset(trace, 0, sizeof(*trace));
    trace->flash = flash;
    trace->last_ns = ffsv_flash_time_ns(flash);
}

static void trace_add_op(struct trace_capture *trace,
        const enum fffs_profile_scope *scope_stack, size_t scope_depth,
        enum fffs_profile_flash_op op, size_t size, uint64_t delta_ns) {
    if (!trace) {
        return;
    }
    for (size_t i = 0; i < scope_depth; i++) {
        enum fffs_profile_scope scope = scope_stack[i];
        if (scope >= FFFS_PROFILE_COUNT) {
            continue;
        }
        struct scope_profile *s = &trace->scopes[scope];
        s->calls++;
        s->bytes += size;
        s->ns += delta_ns;
        if (op == FFFS_PROFILE_FLASH_READ) {
            s->reads++;
        } else if (op == FFFS_PROFILE_FLASH_PROGRAM) {
            s->programs++;
        } else if (op == FFFS_PROFILE_FLASH_ERASE) {
            s->erases++;
        }
    }
}

static void trace_callback(struct fffs *fs,
        const enum fffs_profile_scope *scope_stack, size_t scope_depth,
        enum fffs_profile_flash_op op, size_t offset, size_t size,
        void *user) {
    (void)fs;
    (void)offset;
    struct trace_capture *primary = user;
    if (!primary) {
        primary = active_trace;
    }
    if (!primary) {
        return;
    }
    uint64_t now = ffsv_flash_time_ns(primary->flash);
    uint64_t delta_ns = now - primary->last_ns;
    primary->last_ns = now;
    trace_add_op(primary, scope_stack, scope_depth, op, size, delta_ns);
    if (active_extra_trace) {
        active_extra_trace->last_ns = now;
        trace_add_op(active_extra_trace, scope_stack, scope_depth, op, size,
                delta_ns);
    }
}

static void print_scope_profile(const char *label,
        const struct trace_capture *trace) {
    printf("%s inclusive scope profile\n", label);
    for (size_t i = 0; i < FFFS_PROFILE_COUNT; i++) {
        const struct scope_profile *s = &trace->scopes[i];
        if (s->calls == 0) {
            continue;
        }
        double ms = (double)s->ns / 1000000.0;
        printf("  %-19s calls=%6llu bytes=%9llu time=%9.3f ms "
               "ops=r%llu/p%llu/e%llu\n",
               scope_name((enum fffs_profile_scope)i),
               (unsigned long long)s->calls,
               (unsigned long long)s->bytes,
               ms,
               (unsigned long long)s->reads,
               (unsigned long long)s->programs,
               (unsigned long long)s->erases);
    }
}
#endif

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
        uint32_t *cache) {
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    static uint32_t alloc_map[2048];
#endif
    memset(cache, 0, sizeof(cache[0]) * INDEX_CACHE_WORDS);
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    memset(alloc_map, 0, sizeof(alloc_map));
#endif
    return fffs_mount(fs, backend, &(struct fffs_mount_options){
        .index_cache = cache,
        .index_cache_size = sizeof(cache[0]) * INDEX_CACHE_WORDS,
        .index_hash_table_size = INDEX_HASH_TABLE_SIZE,
        .scratch = scratch,
        .scratch_size = sizeof(scratch),
#if FFFS_PROFILE_TRACE
        .profile_trace = trace_callback,
        .profile_trace_user = NULL,
#endif
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
    fill_pattern(io_buffer, size, seed);

    int err = fffs_open(fs, &file, name,
            FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC);
    if (err != FFFS_OK) {
        return err;
    }
    err = fffs_write(&file, io_buffer, size);
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
    for (uint32_t i = 0; i < model->slot_count; ++i) {
        if (!model->slots[i].live) {
            continue;
        }
        bool exists = false;
        int err = fffs_exists(fs, model->slots[i].name, &exists);
        if (err != FFFS_OK) {
            fprintf(stderr, "%s exists %s slot=%u size=%u failed: %s\n",
                    label, model->slots[i].name, (unsigned)i,
                    model->slots[i].size,
                    fffs_status_name(err));
            return err;
        }
        if (!exists) {
            fprintf(stderr, "%s missing live model file name=%s slot=%u "
                    "size=%u class=%s\n",
                    label, model->slots[i].name, (unsigned)i,
                    model->slots[i].size,
                    bench_churn_class_name(model->slots[i].cls));
            return FFFS_ERR_NOT_FOUND;
        }
    }
    return FFFS_OK;
}

static void snapshot_ops(struct ffsv_flash *flash,
        struct op_summary out[FFSV_OP_COUNT]) {
    memset(out, 0, sizeof(out[0]) * FFSV_OP_COUNT);
    const struct ffsv_op_counts *counts = ffsv_flash_counts(flash);
    if (!counts) {
        return;
    }
    for (size_t i = 0; i < FFSV_OP_COUNT; i++) {
        out[i].calls = counts[i].calls;
        out[i].bytes = counts[i].bytes;
    }
}

static void diff_ops(const struct op_summary before[FFSV_OP_COUNT],
        const struct op_summary after[FFSV_OP_COUNT],
        struct op_summary out[FFSV_OP_COUNT]) {
    memset(out, 0, sizeof(out[0]) * FFSV_OP_COUNT);
    for (size_t i = 0; i < FFSV_OP_COUNT; i++) {
        out[i].calls = after[i].calls - before[i].calls;
        out[i].bytes = after[i].bytes - before[i].bytes;
    }
}

static void add_flash_counts(struct class_stats *stats,
        const struct op_summary ops[FFSV_OP_COUNT]) {
    stats->flash_reads += ops[FFSV_OP_READ].calls;
    stats->flash_programs += ops[FFSV_OP_PROGRAM].calls;
    stats->flash_erases += ops[FFSV_OP_ERASE].calls;
    stats->flash_blank_checks += ops[FFSV_OP_BLANK_CHECK].calls;
}

static int execute_churn_delete(struct fffs *fs, struct ffsv_flash *flash,
        bench_churn_model_t *model, const bench_churn_event_t *event,
        struct class_stats delete_stats[BENCH_CHURN_CLASS_COUNT],
        uint32_t *delete_ops) {
    struct op_summary before_ops[FFSV_OP_COUNT];
    struct op_summary after_ops[FFSV_OP_COUNT];
    snapshot_ops(flash, before_ops);
    uint64_t before = ffsv_flash_time_ns(flash);
    int err = fffs_delete_file(fs, event->name);
    uint64_t after = ffsv_flash_time_ns(flash);
    snapshot_ops(flash, after_ops);
    if (err != FFFS_OK) {
        fprintf(stderr, "delete %s failed: %s\n", event->name,
                fffs_status_name(err));
        return err;
    }

    struct op_summary ops[FFSV_OP_COUNT];
    diff_ops(before_ops, after_ops, ops);
    struct class_stats *s = &delete_stats[event->cls];
    s->ops++;
    s->bytes += event->size;
    s->ns += after - before;
    add_flash_counts(s, ops);
    bench_churn_model_apply(model, event);
    (*delete_ops)++;
    return FFFS_OK;
}

static const char *profile_bucket_name(enum profile_bucket bucket) {
    switch (bucket) {
    case PROFILE_INDEX_READ:
        return "index read";
    case PROFILE_INDEX_PROGRAM:
        return "index program";
    case PROFILE_MD_TAIL_READ:
        return "metadata tail read";
    case PROFILE_MD_TAIL_PROGRAM:
        return "metadata tail program";
    case PROFILE_FOOTER_READ:
        return "footer read";
    case PROFILE_FOOTER_PROGRAM:
        return "footer program";
    case PROFILE_FOOTER_TOMBSTONE:
        return "footer tombstone";
    case PROFILE_SECTOR_SCAN_READ:
        return "sector scan read";
    case PROFILE_DATA_READ:
        return "data read";
    case PROFILE_DATA_PROGRAM:
        return "data program";
    case PROFILE_ERASE:
        return "erase";
    case PROFILE_OTHER:
        return "other";
    default:
        return "unknown";
    }
}

static bool op_is_at_sector_offset(const struct fffs *fs,
        const struct ffsv_op_record *r, size_t pos, size_t size) {
    return r->offset % fs->sector_size == pos && r->size == size;
}

static enum profile_bucket classify_profile_op(const struct fffs *fs,
        const struct ffsv_op_record *r) {
    if (r->type == FFSV_OP_ERASE) {
        return PROFILE_ERASE;
    }

    if (r->sector < fs->index_sectors) {
        if (r->type == FFSV_OP_READ) {
            return PROFILE_INDEX_READ;
        }
        if (r->type == FFSV_OP_PROGRAM) {
            return PROFILE_INDEX_PROGRAM;
        }
    }

    size_t md_tail_size = FFFS_MD_SIZE + FFFS_SECTOR_FOOTER_SIZE;
    size_t md_tail_pos = fs->sector_size - md_tail_size;
    size_t footer_pos = fs->sector_size - FFFS_SECTOR_FOOTER_SIZE;
    size_t tombstone_pos = footer_pos + 4u;

    if (r->type == FFSV_OP_READ &&
            op_is_at_sector_offset(fs, r, md_tail_pos, md_tail_size)) {
        return PROFILE_MD_TAIL_READ;
    }
    if (r->type == FFSV_OP_PROGRAM &&
            op_is_at_sector_offset(fs, r, md_tail_pos, md_tail_size)) {
        return PROFILE_MD_TAIL_PROGRAM;
    }
    if (r->type == FFSV_OP_READ &&
            op_is_at_sector_offset(fs, r, footer_pos,
                    FFFS_SECTOR_FOOTER_SIZE)) {
        return PROFILE_FOOTER_READ;
    }
    if (r->type == FFSV_OP_PROGRAM &&
            op_is_at_sector_offset(fs, r, footer_pos,
                    FFFS_SECTOR_FOOTER_SIZE)) {
        return PROFILE_FOOTER_PROGRAM;
    }
    if (r->type == FFSV_OP_PROGRAM &&
            op_is_at_sector_offset(fs, r, tombstone_pos, 4u)) {
        return PROFILE_FOOTER_TOMBSTONE;
    }
    if (r->type == FFSV_OP_READ && r->sector >= fs->index_sectors &&
            r->offset % fs->sector_size == 0 && r->size == fs->sector_size) {
        return PROFILE_SECTOR_SCAN_READ;
    }
    if (r->type == FFSV_OP_READ) {
        return PROFILE_DATA_READ;
    }
    if (r->type == FFSV_OP_PROGRAM) {
        return PROFILE_DATA_PROGRAM;
    }
    return PROFILE_OTHER;
}

static void summarize_profile(struct ffsv_flash *flash,
        const struct fffs *fs, uint64_t before_seq, uint64_t after_seq,
        struct profile_stats out[PROFILE_COUNT]) {
    memset(out, 0, sizeof(out[0]) * PROFILE_COUNT);
    size_t count = 0;
    const struct ffsv_op_record *log = ffsv_flash_log(flash, &count);
    for (size_t i = 0; i < count; i++) {
        const struct ffsv_op_record *r = &log[i];
        if (r->sequence < before_seq || r->sequence >= after_seq) {
            continue;
        }
        enum profile_bucket bucket = classify_profile_op(fs, r);
        out[bucket].calls += 1;
        out[bucket].bytes += r->committed_bytes != 0 ?
            r->committed_bytes : r->size;
        out[bucket].ns += r->time_after_ns - r->time_before_ns;
    }
}

static void add_profile(struct profile_stats dst[PROFILE_COUNT],
        const struct profile_stats src[PROFILE_COUNT]) {
    for (size_t i = 0; i < PROFILE_COUNT; i++) {
        dst[i].calls += src[i].calls;
        dst[i].bytes += src[i].bytes;
        dst[i].ns += src[i].ns;
    }
}

static void print_profile(const char *label,
        const struct profile_stats stats[PROFILE_COUNT]) {
    uint64_t total_ns = 0;
    for (size_t i = 0; i < PROFILE_COUNT; i++) {
        total_ns += stats[i].ns;
    }
    printf("%s flash profile\n", label);
    for (size_t i = 0; i < PROFILE_COUNT; i++) {
        if (stats[i].calls == 0) {
            continue;
        }
        double ms = (double)stats[i].ns / 1000000.0;
        double pct = total_ns == 0 ? 0.0 :
            ((double)stats[i].ns * 100.0) / (double)total_ns;
        printf("  %-21s calls=%6llu bytes=%9llu time=%9.3f ms %5.1f%%\n",
               profile_bucket_name((enum profile_bucket)i),
               (unsigned long long)stats[i].calls,
               (unsigned long long)stats[i].bytes,
               ms, pct);
    }
}

static void print_wear_stats(struct ffsv_flash *flash, const char *label,
        size_t first_sector) {
    const struct ffsv_flash_config *cfg = ffsv_flash_config(flash);
    size_t sector_count = cfg->total_size / cfg->sector_size;
    if (first_sector >= sector_count) {
        return;
    }

    uint32_t min = UINT32_MAX;
    uint32_t max = 0;
    uint64_t total = 0;
    for (size_t sector = first_sector; sector < sector_count; sector++) {
        uint32_t wear = ffsv_flash_sector_wear(flash, sector);
        if (wear < min) {
            min = wear;
        }
        if (wear > max) {
            max = wear;
        }
        total += wear;
    }

    size_t count = sector_count - first_sector;
    printf("%-24s sectors=%zu min=%u max=%u avg=%.2f\n",
            label, count, min, max, (double)total / (double)count);
}

static const char *class_name(bench_churn_class_t cls) {
    const bench_churn_profile_t *profile = host_churn_profile();
    if ((int)cls >= 0 && cls < BENCH_CHURN_CLASS_COUNT &&
            profile->classes[cls].name) {
        return profile->classes[cls].name;
    }
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
    for (uint32_t i = 0; i < model->slot_count; ++i) {
        if (model->slots[i].live) {
            live++;
        }
    }
    return live;
}

static int run_gc_steps(struct fffs *fs, size_t steps, uint64_t *out_ns,
        size_t *out_erased, struct profile_stats profile[PROFILE_COUNT]
#if FFFS_PROFILE_TRACE
        , struct trace_capture *scope_trace
#endif
        ) {
    uint64_t before = 0;
    uint64_t after = 0;
    uint64_t before_seq = 0;
    uint64_t after_seq = 0;
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
#if FFFS_PROFILE_TRACE
    struct trace_capture *previous_extra = active_extra_trace;
    if (scope_trace) {
        scope_trace->last_ns = ffsv_flash_time_ns(fs->backend.ctx);
        active_extra_trace = scope_trace;
    }
#endif
    before_seq = ffsv_flash_next_sequence(fs->backend.ctx);
    before = ffsv_flash_time_ns(fs->backend.ctx);
    int err = fffs_gc(fs, steps, &erased);
    after = ffsv_flash_time_ns(fs->backend.ctx);
    after_seq = ffsv_flash_next_sequence(fs->backend.ctx);
#if FFFS_PROFILE_TRACE
    active_extra_trace = previous_extra;
#endif
    if (out_ns) {
        *out_ns = after - before;
    }
    if (out_erased) {
        *out_erased = erased;
    }
    if (profile) {
        struct profile_stats local[PROFILE_COUNT];
        summarize_profile(fs->backend.ctx, fs, before_seq, after_seq, local);
        add_profile(profile, local);
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

static void print_no_space_diagnostics(struct fffs *fs,
        const bench_churn_model_t *model, const bench_churn_event_t *event) {
    struct fffs_fsinfo info;
    int err = fffs_fsinfo(fs, &info, FFFS_FSINFO_REFRESH_IF_NEEDED |
            FFFS_FSINFO_ESTIMATE_METADATA);
    if (err != FFFS_OK) {
        fprintf(stderr, "no_space fsinfo failed: %s\n",
                fffs_status_name(err));
        return;
    }

    bool total_valid =
        (info.valid_flags & FFFS_FSINFO_TOTAL_VALID) != 0;
    bool committed_bytes_valid =
        (info.valid_flags & FFFS_FSINFO_COMMITTED_BYTES_VALID) != 0;
    bool inflight_valid =
        (info.valid_flags & FFFS_FSINFO_INFLIGHT_VALID) != 0;
    bool metadata_valid =
        (info.valid_flags & FFFS_FSINFO_METADATA_ESTIMATE_VALID) != 0;
    bool file_count_valid =
        (info.valid_flags & FFFS_FSINFO_COMMITTED_FILES_VALID) != 0;
    uint32_t used = 0;
    bool used_valid = committed_bytes_valid && inflight_valid;
    if (used_valid) {
        used = info.committed_data_bytes + info.inflight_data_bytes;
        if (metadata_valid) {
            used += info.estimated_metadata_bytes;
        }
    }
    uint32_t fs_free = total_valid && used_valid && info.total_bytes > used ?
        info.total_bytes - used : 0;
    bool fs_free_valid = total_valid && used_valid;
    uint32_t model_limit =
        model->target_live_bytes + model->target_slack_bytes;
    uint32_t model_free = model_limit > model->live_bytes ?
        model_limit - model->live_bytes : 0;

    fprintf(stderr,
            "no_space fsinfo valid=0x%08x total_valid=%d used_valid=%d file_count_valid=%d metadata_valid=%d total=%u used_est=%u fs_free_valid=%d fs_free_est=%u files=%u committed_data=%u inflight_files=%u inflight_data=%u metadata=%u request=%u model_live=%u model_live_limit=%u model_free_budget=%u fragmentation_hint=%d\n",
            info.valid_flags, total_valid, used_valid, file_count_valid,
            metadata_valid, info.total_bytes, used, fs_free_valid, fs_free,
            info.committed_file_count, info.committed_data_bytes,
            info.inflight_file_count, info.inflight_data_bytes,
            info.estimated_metadata_bytes, event->size, model->live_bytes,
            model_limit, model_free,
            fs_free_valid && fs_free >= event->size &&
                model_free >= event->size);
    fprintf(stderr,
            "no_space progress events=%u writes=%u deletes=%u creates=%u replaces=%u live_files=%u live_bytes=%u total_written=%u target_written=%u pending_slot=%d pending_replacing=%d request=%u\n",
            model->op_count + model->delete_count, model->op_count,
            model->delete_count, model->create_count, model->replace_count,
            model->live_file_count, model->live_bytes, model->total_written,
            model->target_written_bytes, event->slot, event->replacing,
            event->size);
    size_t count = 0;
    for (size_t i = 0; i < FFFS_COMPACTION_CANDIDATE_COUNT; i++) {
        if (fs->compaction_candidates[i].sector != 0) {
            count++;
        }
    }
    fprintf(stderr, "no_space compaction_candidates count=%zu", count);
    for (size_t i = 0; i < FFFS_COMPACTION_CANDIDATE_COUNT; i++) {
        const struct fffs_compaction_candidate *candidate =
            &fs->compaction_candidates[i];
        if (candidate->sector == 0) {
            continue;
        }
        fprintf(stderr, " sector%zu=%u trapped%zu=%u roots%zu=%u",
                i, candidate->sector, i, candidate->trapped_reclaimable,
                i, candidate->live_root_count);
    }
    fprintf(stderr, "\n");
}

static const bench_churn_profile_t *host_churn_profile(void) {
#if FFFS_HOST_CHURN_PROFILE_SMALL_FILES
    static const bench_churn_profile_t profile = {
        .name_prefix = "sf",
        .replace_percent = 25,
        .protect_first_large = false,
        .classes = {
            [BENCH_CHURN_CLASS_SMALL] = {
                .name = "small_1_5k",
                .weight = 1000,
                .min_size = FFFS_HOST_CHURN_SMALL_MIN_SIZE,
                .max_size = FFFS_HOST_CHURN_SMALL_MAX_SIZE,
            },
        },
    };
    return &profile;
#else
    return bench_churn_default_profile();
#endif
}

static int init_host_churn_model(bench_churn_model_t *model) {
    return bench_churn_model_init_profile(model, FFFS_HOST_CHURN_SEED,
            FFFS_HOST_CHURN_TARGET_LIVE_BYTES,
            FFFS_HOST_CHURN_TARGET_WRITTEN_BYTES,
            FFFS_HOST_CHURN_TARGET_SLACK_BYTES,
            FFFS_HOST_CHURN_FORCE_LARGE_AFTER_BYTES,
            host_churn_profile(), model_slots, FFFS_HOST_CHURN_MAX_FILES);
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
    struct profile_stats gc_profile[PROFILE_COUNT] = {0};
#if FFFS_PROFILE_TRACE
    struct trace_capture churn_scope_profile;
    struct trace_capture gc_scope_profile;
    struct trace_capture list_scope_profile;
    struct trace_capture mount_scope_profile;
    struct trace_capture read_scope_profile;
#endif
    uint32_t write_ops = 0;
    uint32_t delete_ops = 0;
    uint32_t no_space_retries = 0;
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
    fflush(stdout);

    uint64_t format_before = ffsv_flash_time_ns(flash);
    err = fffs_format(&backend, NULL);
    uint64_t format_after = ffsv_flash_time_ns(flash);
    if (err != FFFS_OK) {
        fprintf(stderr, "format failed: %s\n", fffs_status_name(err));
        dump_final_image(flash, profile_name);
        ffsv_flash_destroy(flash);
        return 1;
    }
#if FFFS_PROFILE_TRACE
    trace_reset(&mount_scope_profile, flash);
    active_trace = &mount_scope_profile;
#endif
    err = mount_fs(&fs, &backend, index_cache);
#if FFFS_PROFILE_TRACE
    active_trace = NULL;
#endif
    if (err != FFFS_OK) {
        fprintf(stderr, "mount failed: %s\n", fffs_status_name(err));
        dump_final_image(flash, profile_name);
        ffsv_flash_destroy(flash);
        return 1;
    }

    memset(slot_seed, 0, sizeof(slot_seed));
    err = init_host_churn_model(&model);
    if (err != 0) {
        fprintf(stderr, "churn model init failed\n");
        fffs_unmount(&fs);
        dump_final_image(flash, profile_name);
        ffsv_flash_destroy(flash);
        return 1;
    }

#if FFFS_PROFILE_TRACE
    trace_reset(&churn_scope_profile, flash);
    trace_reset(&gc_scope_profile, flash);
    active_trace = &churn_scope_profile;
#endif
    uint64_t churn_before_seq = ffsv_flash_next_sequence(flash);
    struct op_summary churn_before_ops[FFSV_OP_COUNT];
    snapshot_ops(flash, churn_before_ops);
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

        if (type == BENCH_CHURN_EVENT_DELETE) {
            err = execute_churn_delete(&fs, flash, &model, &event,
                    delete_stats, &delete_ops);
            if (err != FFFS_OK) {
                break;
            }
        } else {
            struct op_summary before_ops[FFSV_OP_COUNT];
            struct op_summary after_ops[FFSV_OP_COUNT];
            snapshot_ops(flash, before_ops);
            uint64_t before = ffsv_flash_time_ns(flash);
            err = write_file(&fs, event.name, event.size, event.write_seed);
            if (err == FFFS_ERR_NO_SPACE) {
                no_space_retries++;
            }
            uint64_t after = ffsv_flash_time_ns(flash);
            snapshot_ops(flash, after_ops);
            if (err != FFFS_OK) {
                if (err == FFFS_ERR_NO_SPACE) {
                    print_no_space_diagnostics(&fs, &model, &event);
                }
                fprintf(stderr, "write %s size=%u failed: %s\n", event.name,
                        event.size, fffs_status_name(err));
                break;
            }
            struct op_summary ops[FFSV_OP_COUNT];
            diff_ops(before_ops, after_ops, ops);
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
                &local_gc_ns, &local_erased, gc_profile
#if FFFS_PROFILE_TRACE
                , &gc_scope_profile
#endif
                );
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
    struct op_summary churn_after_ops[FFSV_OP_COUNT];
    snapshot_ops(flash, churn_after_ops);
#if FFFS_PROFILE_TRACE
    active_trace = NULL;
#endif

    if (err != FFFS_OK) {
        fffs_unmount(&fs);
        dump_final_image(flash, profile_name);
        ffsv_flash_destroy(flash);
        return 1;
    }

    size_t listed = 0;
#if FFFS_PROFILE_TRACE
    trace_reset(&list_scope_profile, flash);
    active_trace = &list_scope_profile;
#endif
    uint64_t list_before_seq = ffsv_flash_next_sequence(flash);
    struct op_summary list_before_ops[FFSV_OP_COUNT];
    snapshot_ops(flash, list_before_ops);
    uint64_t list_before = ffsv_flash_time_ns(flash);
    err = list_all(&fs, &listed);
    uint64_t list_after = ffsv_flash_time_ns(flash);
    uint64_t list_after_seq = ffsv_flash_next_sequence(flash);
    struct op_summary list_after_ops[FFSV_OP_COUNT];
    snapshot_ops(flash, list_after_ops);
#if FFFS_PROFILE_TRACE
    active_trace = NULL;
#endif
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

#if FFFS_PROFILE_TRACE
    trace_reset(&mount_scope_profile, flash);
    active_trace = &mount_scope_profile;
#endif
    uint64_t mount_before_seq = ffsv_flash_next_sequence(flash);
    struct op_summary mount_before_ops[FFSV_OP_COUNT];
    snapshot_ops(flash, mount_before_ops);
    uint64_t mount_before = ffsv_flash_time_ns(flash);
    err = mount_fs(&remounted, &backend, remount_cache);
    uint64_t mount_after = ffsv_flash_time_ns(flash);
    uint64_t mount_after_seq = ffsv_flash_next_sequence(flash);
    struct op_summary mount_after_ops[FFSV_OP_COUNT];
    snapshot_ops(flash, mount_after_ops);
#if FFFS_PROFILE_TRACE
    active_trace = NULL;
#endif
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

#if FFFS_PROFILE_TRACE
    trace_reset(&read_scope_profile, flash);
    active_trace = &read_scope_profile;
#endif
    uint64_t read_before_seq = ffsv_flash_next_sequence(flash);
    struct op_summary read_before_ops[FFSV_OP_COUNT];
    snapshot_ops(flash, read_before_ops);
    uint64_t read_before = ffsv_flash_time_ns(flash);
    size_t read_files = 0;
    for (uint32_t pass = 0; pass < model.slot_count; ++pass) {
        uint32_t slot = (pass * 73u + 19u) % model.slot_count;
        if (!model.slots[slot].live) {
            continue;
        }
        bench_churn_slot_t *s = &model.slots[slot];
        struct op_summary before_ops[FFSV_OP_COUNT];
        struct op_summary after_ops[FFSV_OP_COUNT];
        snapshot_ops(flash, before_ops);
        uint64_t before = ffsv_flash_time_ns(flash);
        err = read_file(&remounted, s->name, s->size, slot_seed[slot]);
        uint64_t after = ffsv_flash_time_ns(flash);
        snapshot_ops(flash, after_ops);
        if (err != FFFS_OK) {
            fprintf(stderr, "read %s failed: %s\n", s->name,
                    fffs_status_name(err));
            fffs_unmount(&remounted);
            dump_final_image(flash, profile_name);
            ffsv_flash_destroy(flash);
            return 1;
        }
        struct op_summary ops[FFSV_OP_COUNT];
        diff_ops(before_ops, after_ops, ops);
        struct class_stats *rs = &read_stats[s->cls];
        rs->ops++;
        rs->bytes += s->size;
        rs->ns += after - before;
        add_flash_counts(rs, ops);
        read_files++;
    }
    uint64_t read_after = ffsv_flash_time_ns(flash);
    uint64_t read_after_seq = ffsv_flash_next_sequence(flash);
    struct op_summary read_after_ops[FFSV_OP_COUNT];
    snapshot_ops(flash, read_after_ops);
#if FFFS_PROFILE_TRACE
    active_trace = NULL;
#endif

    struct op_summary churn_ops[FFSV_OP_COUNT];
    struct op_summary list_ops[FFSV_OP_COUNT];
    struct op_summary mount_ops[FFSV_OP_COUNT];
    struct op_summary read_ops[FFSV_OP_COUNT];
    struct profile_stats churn_profile[PROFILE_COUNT];
    struct profile_stats list_profile[PROFILE_COUNT];
    struct profile_stats mount_profile[PROFILE_COUNT];
    struct profile_stats read_profile[PROFILE_COUNT];
    diff_ops(churn_before_ops, churn_after_ops, churn_ops);
    diff_ops(list_before_ops, list_after_ops, list_ops);
    diff_ops(mount_before_ops, mount_after_ops, mount_ops);
    diff_ops(read_before_ops, read_after_ops, read_ops);
    summarize_profile(flash, &fs, churn_before_seq, churn_after_seq,
            churn_profile);
    summarize_profile(flash, &fs, list_before_seq, list_after_seq,
            list_profile);
    summarize_profile(flash, &remounted, mount_before_seq, mount_after_seq,
            mount_profile);
    summarize_profile(flash, &remounted, read_before_seq, read_after_seq,
            read_profile);

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
    print_profile("churn total", churn_profile);
    print_profile("gc idle", gc_profile);
#if FFFS_PROFILE_TRACE
    print_scope_profile("churn total", &churn_scope_profile);
    print_scope_profile("gc idle", &gc_scope_profile);
#endif
    print_class_stats("write", write_stats);
    print_class_stats("delete", delete_stats);
    printf("warm list               files=%zu time=%9.3f ms flash_ops=r%llu/p%llu/e%llu/bc%llu\n",
           listed, (double)(list_after - list_before) / 1000000.0,
           (unsigned long long)list_ops[FFSV_OP_READ].calls,
           (unsigned long long)list_ops[FFSV_OP_PROGRAM].calls,
           (unsigned long long)list_ops[FFSV_OP_ERASE].calls,
           (unsigned long long)list_ops[FFSV_OP_BLANK_CHECK].calls);
    print_profile("warm list", list_profile);
#if FFFS_PROFILE_TRACE
    print_scope_profile("warm list", &list_scope_profile);
#endif
    printf("cold mount              time=%9.3f ms flash_ops=r%llu/p%llu/e%llu/bc%llu\n",
           (double)(mount_after - mount_before) / 1000000.0,
           (unsigned long long)mount_ops[FFSV_OP_READ].calls,
           (unsigned long long)mount_ops[FFSV_OP_PROGRAM].calls,
           (unsigned long long)mount_ops[FFSV_OP_ERASE].calls,
           (unsigned long long)mount_ops[FFSV_OP_BLANK_CHECK].calls);
    print_profile("cold mount", mount_profile);
#if FFFS_PROFILE_TRACE
    print_scope_profile("cold mount", &mount_scope_profile);
#endif
    printf("cold read summary       files=%zu time=%9.3f ms flash_ops=r%llu/p%llu/e%llu/bc%llu\n",
           read_files, (double)(read_after - read_before) / 1000000.0,
           (unsigned long long)read_ops[FFSV_OP_READ].calls,
           (unsigned long long)read_ops[FFSV_OP_PROGRAM].calls,
           (unsigned long long)read_ops[FFSV_OP_ERASE].calls,
           (unsigned long long)read_ops[FFSV_OP_BLANK_CHECK].calls);
    print_profile("cold read", read_profile);
#if FFFS_PROFILE_TRACE
    print_scope_profile("cold read", &read_scope_profile);
#endif
    print_class_stats("cold read", read_stats);
    print_wear_stats(flash, "wear all sectors", 0);
    print_wear_stats(flash, "wear data sectors", remounted.index_sectors);

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
