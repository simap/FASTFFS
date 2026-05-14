#include "fastffs/fastffs.h"
#include "fastffs/fastffs_host.h"
#include "fastffs/verify_flash.h"

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

#ifndef FFFS_TIME_PROBE_SCRATCH_SIZE
#define FFFS_TIME_PROBE_SCRATCH_SIZE 4096
#endif

#if FFFS_TIME_PROBE_SCRATCH_SIZE < FFFS_MIN_SCRATCH_SIZE
#error "FFFS_TIME_PROBE_SCRATCH_SIZE must be at least FFFS_MIN_SCRATCH_SIZE"
#endif

static uint16_t index_heads[INDEX_HASH_TABLE_SIZE];
static uint16_t remount_heads[INDEX_HASH_TABLE_SIZE];
static uint8_t scratch[FFFS_TIME_PROBE_SCRATCH_SIZE];

struct op_summary {
    uint64_t calls;
    uint64_t bytes;
    uint64_t ns;
};

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

static void format_duration(char *out, size_t out_size, uint64_t ns) {
    if (ns >= 1000000) {
        snprintf(out, out_size, "%.3f ms", (double)ns / 1000000.0);
    } else {
        snprintf(out, out_size, "%.0f us", (double)ns / 1000.0);
    }
}

static void format_bytes(char *out, size_t out_size, uint64_t bytes) {
    if (bytes != 0 && bytes % (1024 * 1024) == 0) {
        snprintf(out, out_size, "%lluMiB",
                (unsigned long long)(bytes / (1024 * 1024)));
    } else if (bytes != 0 && bytes % 1024 == 0 && bytes >= 1024 * 1024) {
        snprintf(out, out_size, "%lluKiB",
                (unsigned long long)(bytes / 1024));
    } else {
        snprintf(out, out_size, "%lluB", (unsigned long long)bytes);
    }
}

static void append_op(char *out, size_t out_size, bool *first,
        const char *label, const struct op_summary *s) {
    if (s->calls == 0) {
        return;
    }
    char duration[32];
    char bytes[32];
    format_duration(duration, sizeof(duration), s->ns);
    format_bytes(bytes, sizeof(bytes), s->bytes);
    size_t len = strlen(out);
    snprintf(out + len, out_size - len, "%s%s=%llu/%s/%s",
            *first ? "" : ", ", label,
            (unsigned long long)s->calls,
            bytes, duration);
    *first = false;
}

static void print_delta(struct ffsv_flash *flash, const char *name,
        uint64_t before_time, uint64_t after_time, uint64_t before_seq,
        uint64_t after_seq) {
    struct op_summary ops[FFSV_OP_COUNT];
    summarize_ops(flash, before_seq, after_seq, ops);
    char duration[32];
    char op_summary[160] = "";
    bool first = true;
    format_duration(duration, sizeof(duration), after_time - before_time);
    append_op(op_summary, sizeof(op_summary), &first, "r",
            &ops[FFSV_OP_READ]);
    append_op(op_summary, sizeof(op_summary), &first, "p",
            &ops[FFSV_OP_PROGRAM]);
    append_op(op_summary, sizeof(op_summary), &first, "e",
            &ops[FFSV_OP_ERASE]);
    append_op(op_summary, sizeof(op_summary), &first, "bc",
            &ops[FFSV_OP_BLANK_CHECK]);
    printf("%-20s %10s   %s\n", name, duration,
            first ? "no flash ops" : op_summary);
}

static int mount_fs(struct fffs *fs, const struct fffs_backend *backend,
        uint16_t *heads) {
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    static uint32_t alloc_map[2048];
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

static int write_file(struct fffs *fs, const char *name, const void *data,
        size_t size) {
    struct fffs_file file;
    size_t written;
    int err = fffs_open(fs, &file, name,
            FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC);
    if (err != FFFS_OK) {
        return err;
    }
    err = fffs_write(&file, data, size, &written);
    if (err != FFFS_OK || written != size) {
        return err == FFFS_OK ? FFFS_ERR_IO : err;
    }
    return fffs_close(&file);
}

static int read_file(struct fffs *fs, const char *name, void *data,
        size_t size, size_t *out_read) {
    struct fffs_file file;
    int err = fffs_open(fs, &file, name, FFFS_O_RDONLY);
    if (err != FFFS_OK) {
        return err;
    }
    err = fffs_read(&file, data, size, out_read);
    if (err != FFFS_OK) {
        return err;
    }
    return fffs_close(&file);
}

static int run_probe(enum ffsv_flash_preset preset, const char *profile_name) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    memset(index_heads, 0, sizeof(index_heads));
    memset(remount_heads, 0, sizeof(remount_heads));

    printf("%s [%s, scratch=%u B]\n", profile_name, cache_mode_name(),
            (unsigned)FFFS_TIME_PROBE_SCRATCH_SIZE);
    printf("%-20s %10s   %s\n", "operation", "time", "flash ops");
    printf("%-20s %10s   %s\n", "---------", "----", "---------");

    struct ffsv_flash_config cfg;
    int err = ffsv_flash_config_preset(&cfg, preset, 4096 * 256);
    if (err != FFSV_OK) {
        return 1;
    }
    cfg.max_log_entries = 65536;
    err = ffsv_flash_create(&flash, &cfg);
    if (err != FFSV_OK) {
        return 1;
    }
    err = fffs_host_backend_from_verify_flash(&backend, flash);
    if (err != FFFS_OK) {
        return 1;
    }

    uint64_t before_seq = ffsv_flash_next_sequence(flash);
    uint64_t before = ffsv_flash_time_ns(flash);
    err = fffs_format(&backend, NULL);
    uint64_t after = ffsv_flash_time_ns(flash);
    uint64_t after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        return 1;
    }
    print_delta(flash, "format", before, after, before_seq, after_seq);

    before_seq = ffsv_flash_next_sequence(flash);
    before = ffsv_flash_time_ns(flash);
    err = mount_fs(&fs, &backend, index_heads);
    after = ffsv_flash_time_ns(flash);
    after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        return 1;
    }
    print_delta(flash, "mount empty", before, after, before_seq, after_seq);

    const char small[] = "hello";
    before_seq = ffsv_flash_next_sequence(flash);
    before = ffsv_flash_time_ns(flash);
    err = write_file(&fs, "config", small, strlen(small));
    after = ffsv_flash_time_ns(flash);
    after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        return 1;
    }
    print_delta(flash, "write 5 B new", before, after, before_seq, after_seq);

    struct fffs_stat st;
    before_seq = ffsv_flash_next_sequence(flash);
    before = ffsv_flash_time_ns(flash);
    err = fffs_stat(&fs, "config", &st);
    after = ffsv_flash_time_ns(flash);
    after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        return 1;
    }
    print_delta(flash, "stat existing", before, after, before_seq, after_seq);

    struct fffs_file open_file;
    before_seq = ffsv_flash_next_sequence(flash);
    before = ffsv_flash_time_ns(flash);
    err = fffs_open(&fs, &open_file, "config", FFFS_O_RDONLY);
    if (err == FFFS_OK) {
        err = fffs_close(&open_file);
    }
    after = ffsv_flash_time_ns(flash);
    after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        return 1;
    }
    print_delta(flash, "open existing", before, after, before_seq, after_seq);

    char out[32];
    size_t out_read;
    before_seq = ffsv_flash_next_sequence(flash);
    before = ffsv_flash_time_ns(flash);
    err = read_file(&fs, "config", out, sizeof(out), &out_read);
    after = ffsv_flash_time_ns(flash);
    after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        return 1;
    }
    print_delta(flash, "open+read 5 B", before, after, before_seq,
            after_seq);

    before_seq = ffsv_flash_next_sequence(flash);
    before = ffsv_flash_time_ns(flash);
    err = fffs_exists(&fs, "missing", &(bool){0});
    after = ffsv_flash_time_ns(flash);
    after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        return 1;
    }
    print_delta(flash, "exists missing", before, after, before_seq,
            after_seq);

    const char small2[] = "world";
    before_seq = ffsv_flash_next_sequence(flash);
    before = ffsv_flash_time_ns(flash);
    err = write_file(&fs, "config", small2, strlen(small2));
    after = ffsv_flash_time_ns(flash);
    after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        return 1;
    }
    print_delta(flash, "overwrite 5 B", before, after, before_seq,
            after_seq);

    before_seq = ffsv_flash_next_sequence(flash);
    before = ffsv_flash_time_ns(flash);
    err = fffs_delete_file(&fs, "config");
    after = ffsv_flash_time_ns(flash);
    after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        return 1;
    }
    print_delta(flash, "delete existing", before, after, before_seq,
            after_seq);

    for (int i = 0; i < 100; i++) {
        char name[24];
        char value[16];
        snprintf(name, sizeof(name), "file-%03d", i);
        snprintf(value, sizeof(value), "v%03d", i);
        err = write_file(&fs, name, value, strlen(value));
        if (err != FFFS_OK) {
            return 1;
        }
    }

    before_seq = ffsv_flash_next_sequence(flash);
    before = ffsv_flash_time_ns(flash);
    struct fffs_dir dir;
    struct fffs_stat dst;
    err = fffs_dir_open(&fs, &dir, "");
    if (err != FFFS_OK) {
        return 1;
    }
    size_t listed = 0;
    while (fffs_dir_read(&dir, &dst)) {
        listed++;
    }
    after = ffsv_flash_time_ns(flash);
    after_seq = ffsv_flash_next_sequence(flash);
    print_delta(flash, "list 100 files", before, after, before_seq,
            after_seq);
    (void)listed;

    fffs_unmount(&fs);
    before_seq = ffsv_flash_next_sequence(flash);
    before = ffsv_flash_time_ns(flash);
    err = mount_fs(&remounted, &backend, remount_heads);
    after = ffsv_flash_time_ns(flash);
    after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        return 1;
    }
    print_delta(flash, "mount 100 files", before, after, before_seq,
            after_seq);

    uint8_t *large = malloc(24 * 1024);
    uint8_t *large_out = malloc(24 * 1024);
    if (!large || !large_out) {
        return 1;
    }
    for (size_t i = 0; i < 24 * 1024; i++) {
        large[i] = (uint8_t)(i * 17u + i / 3u);
    }
    before_seq = ffsv_flash_next_sequence(flash);
    before = ffsv_flash_time_ns(flash);
    err = write_file(&remounted, "large.bin", large, 24 * 1024);
    after = ffsv_flash_time_ns(flash);
    after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        return 1;
    }
    print_delta(flash, "write 24 KiB", before, after, before_seq, after_seq);

    before_seq = ffsv_flash_next_sequence(flash);
    before = ffsv_flash_time_ns(flash);
    err = read_file(&remounted, "large.bin", large_out, 24 * 1024,
            &out_read);
    after = ffsv_flash_time_ns(flash);
    after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        return 1;
    }
    print_delta(flash, "open+read 24 KiB", before, after, before_seq,
            after_seq);

    enum fffs_gc_action action;
    remounted.gc_cursor = remounted.index_sectors;
    before_seq = ffsv_flash_next_sequence(flash);
    before = ffsv_flash_time_ns(flash);
    err = fffs_gc_step(&remounted, &action);
    after = ffsv_flash_time_ns(flash);
    after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        return 1;
    }
    print_delta(flash, "GC tombstone step", before, after, before_seq,
            after_seq);

    before_seq = ffsv_flash_next_sequence(flash);
    before = ffsv_flash_time_ns(flash);
    err = fffs_gc_step(&remounted, &action);
    after = ffsv_flash_time_ns(flash);
    after_seq = ffsv_flash_next_sequence(flash);
    if (err != FFFS_OK) {
        return 1;
    }
    print_delta(flash, "GC erase step", before, after, before_seq, after_seq);

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    free(large_out);
    free(large);
    return 0;
}

int main(void) {
    int err = run_probe(FFSV_PRESET_TARGET_NOR_NOTES, "target-nor-notes");
    if (err != 0) {
        return err;
    }
    printf("\n");
    return run_probe(FFSV_PRESET_ESP32S3_MEASURED, "esp32s3-measured");
}
