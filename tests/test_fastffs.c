#include "fastffs/fastffs.h"
#include "fastffs/fastffs_host.h"
#include "fastffs/fastffs_inspect.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_OK(expr) do { \
    int rc__ = (expr); \
    if (rc__ != FFFS_OK) { \
        fprintf(stderr, "%s:%d: expected ok, got %s\n", \
                __FILE__, __LINE__, fffs_status_name(rc__)); \
        return 1; \
    } \
} while (0)

#define ASSERT_EQ_INT(exp, got) do { \
    int exp__ = (exp); \
    int got__ = (got); \
    if (exp__ != got__) { \
        fprintf(stderr, "%s:%d: expected %d, got %d\n", \
                __FILE__, __LINE__, exp__, got__); \
        return 1; \
    } \
} while (0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: assertion failed: %s\n", \
                __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

#ifndef FFFS_INDEX_HASH_TABLE_SIZE
#define FFFS_INDEX_HASH_TABLE_SIZE 1024
#endif

#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
#define TEST_INDEX_HASH_TABLE_SIZE FFFS_SLOT_COUNT
#else
#define TEST_INDEX_HASH_TABLE_SIZE FFFS_INDEX_HASH_TABLE_SIZE
#endif

#define TEST_ALLOC_MAP_WORDS 256
#define TEST_SCRATCH_SIZE 4096
#define TEST_INDEX_HEADER_SIZE 8
#define TEST_SECTOR_FOOTER_SIZE 10u
#define TEST_MD_FILE_RECORD_SIZE 16u
#define TEST_MD_FLAGS_VALID 0x7eu
#define TEST_MD_FLAGS_TOMBSTONED 0x3cu
#define TEST_MD_TYPE_FILE_CONT_V1 0x12u
#define TEST_MD_TYPE_UNKNOWN 0x13u
#define TEST_SECTOR_TYPE_FILE 0x01u
#define TEST_SECTOR_FLAGS_VALID 0x7eu
#define TEST_SECTOR_FLAGS_FULL 0x5au
#define TEST_RESERVE_BACKEND_SECTORS \
    (FFFS_DEFAULT_INDEX_SECTORS + FFFS_COMPACTION_RESERVE_SECTORS + \
     FFFS_ALLOC_RESERVE_SECTORS + 4u)
#define TEST_RESERVE_PRESSURE_BACKEND_SECTORS \
    (FFFS_DEFAULT_INDEX_SECTORS + FFFS_COMPACTION_RESERVE_SECTORS + \
     FFFS_ALLOC_RESERVE_SECTORS + 1u)
#define TEST_INDEX_CACHE_WORDS \
    (((FFFS_INDEX_CACHE_BYTES(TEST_INDEX_HASH_TABLE_SIZE) + \
       sizeof(uint32_t) - 1u) / sizeof(uint32_t)) ? \
     ((FFFS_INDEX_CACHE_BYTES(TEST_INDEX_HASH_TABLE_SIZE) + \
       sizeof(uint32_t) - 1u) / sizeof(uint32_t)) : 1u)

typedef uint32_t test_index_cache_t;

uint16_t fffs_hash16(const char *name);
int fffs_index_compact_oldest(struct fffs *fs);
int fffs_rotate_index(struct fffs *fs);
int fffs_gc_until_erased(struct fffs *fs, uint16_t *erased_sector);
int fffs_index_head_for_slot(struct fffs *fs, uint16_t slot,
        uint16_t *head, bool *found);
int fffs_index_insert(struct fffs *fs, uint16_t slot, uint16_t head);
int fffs_index_remove(struct fffs *fs, uint16_t slot);
int fffs_alloc_find_compaction_root_window(struct fffs *fs,
        uint16_t source_sector, uint16_t slot, uint16_t data_len,
        uint16_t *sector, uint16_t *data_off, uint16_t *record_off,
        bool *needs_footer, bool allow_relaxed);
enum fffs_tombstone_accounting {
    FFFS_TOMBSTONE_NO_ACCOUNTING,
    FFFS_TOMBSTONE_COMMITTED_DELETE,
};
int fffs_tombstone_metadata_for_slot(struct fffs *fs, uint16_t sector,
        uint16_t want_slot, enum fffs_tombstone_accounting accounting,
        bool *accounted);
struct fffs_read_cache_view;
struct fffs_md_record {
    uint8_t type;
    uint8_t state;
    bool live;
    uint16_t slot;
    uint16_t next;
    uint16_t span_len;
    uint16_t data_off;
    uint16_t data_len;
    uint32_t size_or_offset;
    size_t record_start;
    size_t record_len;
};
int fffs_read_md_for_slot(struct fffs *fs, uint16_t sector,
        uint16_t want_slot, struct fffs_md_record *out);
int fffs_find_sector_free_window(struct fffs *fs, uint16_t sector,
        uint16_t min_free, uint16_t reject_slot,
        bool normal_allocation, uint16_t *data_off,
        uint16_t *record_off, bool *needs_footer, uint16_t *md_records);

struct measured_ops {
    uint64_t calls[FFSV_OP_COUNT];
    uint64_t bytes[FFSV_OP_COUNT];
    uint64_t ns_by_op[FFSV_OP_COUNT];
    uint64_t ns;
};

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

static void append_measured_op(char *out, size_t out_size, bool *first,
        const char *label, const struct measured_ops *ops,
        enum ffsv_op_type type) {
    if (ops->calls[type] == 0) {
        return;
    }
    char duration[32];
    char bytes[32];
    format_duration(duration, sizeof(duration), ops->ns_by_op[type]);
    format_bytes(bytes, sizeof(bytes), ops->bytes[type]);
    size_t len = strlen(out);
    snprintf(out + len, out_size - len, "%s%s=%llu/%s/%s",
            *first ? "" : ", ", label,
            (unsigned long long)ops->calls[type], bytes, duration);
    *first = false;
}

static int flash_to_fs(int status) {
    return status == FFSV_OK ? FFFS_OK : FFFS_ERR_IO;
}

static void inject_next(struct ffsv_flash *flash, enum ffsv_op_type op,
        enum ffsv_failure_phase phase, size_t partial_bytes) {
    ffsv_flash_set_failure(flash, &(struct ffsv_failure_injection){
        .enabled = true,
        .sequence = ffsv_flash_next_sequence(flash),
        .op_mask = UINT32_C(1) << op,
        .phase = phase,
        .status = FFSV_ERR_INJECTED,
        .partial_bytes = partial_bytes,
    });
}

static void capture_ops(struct ffsv_flash *flash, struct measured_ops *out) {
    const struct ffsv_op_counts *counts = ffsv_flash_counts(flash);
    memset(out, 0, sizeof(*out));
    out->ns = ffsv_flash_time_ns(flash);
    for (size_t i = 0; i < FFSV_OP_COUNT; i++) {
        out->calls[i] = counts[i].calls;
        out->bytes[i] = counts[i].bytes;
    }
}

static void diff_ops(const struct measured_ops *before,
        const struct measured_ops *after, const struct ffsv_timing *timing,
        struct measured_ops *out) {
    memset(out, 0, sizeof(*out));
    out->ns = after->ns - before->ns;
    for (size_t i = 0; i < FFSV_OP_COUNT; i++) {
        out->calls[i] = after->calls[i] - before->calls[i];
        out->bytes[i] = after->bytes[i] - before->bytes[i];
    }
    out->ns_by_op[FFSV_OP_READ] =
        out->calls[FFSV_OP_READ] * timing->read_fixed_ns +
        out->bytes[FFSV_OP_READ] * timing->read_per_byte_ns;
    out->ns_by_op[FFSV_OP_PROGRAM] =
        out->calls[FFSV_OP_PROGRAM] * timing->program_fixed_ns +
        out->bytes[FFSV_OP_PROGRAM] * timing->program_per_byte_ns;
    out->ns_by_op[FFSV_OP_ERASE] =
        out->calls[FFSV_OP_ERASE] * timing->erase_fixed_ns +
        out->bytes[FFSV_OP_ERASE] * timing->erase_per_byte_ns;
    out->ns_by_op[FFSV_OP_BLANK_CHECK] =
        out->calls[FFSV_OP_BLANK_CHECK] * timing->blank_check_fixed_ns +
        out->bytes[FFSV_OP_BLANK_CHECK] * timing->blank_check_per_byte_ns;
    out->ns_by_op[FFSV_OP_STAGE_PROGRAM] =
        out->calls[FFSV_OP_STAGE_PROGRAM] * timing->program_fixed_ns +
        out->bytes[FFSV_OP_STAGE_PROGRAM] * timing->program_per_byte_ns;
    out->ns_by_op[FFSV_OP_COMMIT_STAGED] =
        out->calls[FFSV_OP_COMMIT_STAGED] * timing->program_fixed_ns +
        out->bytes[FFSV_OP_COMMIT_STAGED] * timing->program_per_byte_ns;
    out->ns_by_op[FFSV_OP_DROP_STAGED] =
        out->calls[FFSV_OP_DROP_STAGED] * timing->program_fixed_ns +
        out->bytes[FFSV_OP_DROP_STAGED] * timing->program_per_byte_ns;
}

static int new_backend(struct ffsv_flash **flash,
        struct fffs_backend *backend) {
    int err = ffsv_flash_create_with_preset(flash,
            FFSV_PRESET_TARGET_NOR_NOTES, 4096 * 16);
    if (err != FFSV_OK) {
        return FFFS_ERR_IO;
    }
    return fffs_host_backend_from_verify_flash(backend, *flash);
}

static int new_backend_with_size(struct ffsv_flash **flash,
        struct fffs_backend *backend, size_t size) {
    int err = ffsv_flash_create_with_preset(flash,
            FFSV_PRESET_TARGET_NOR_NOTES, size);
    if (err != FFSV_OK) {
        return FFFS_ERR_IO;
    }
    return fffs_host_backend_from_verify_flash(backend, *flash);
}

static int new_backend_with_sector_size(struct ffsv_flash **flash,
        struct fffs_backend *backend, size_t sector_size,
        size_t sector_count) {
    struct ffsv_flash_config cfg;
    int err = ffsv_flash_config_preset(&cfg, FFSV_PRESET_TARGET_NOR_NOTES,
            sector_size * sector_count);
    if (err != FFSV_OK) {
        return FFFS_ERR_IO;
    }
    cfg.sector_size = sector_size;
    cfg.max_log_entries = 50000;
    err = ffsv_flash_create(flash, &cfg);
    if (err != FFSV_OK) {
        return FFFS_ERR_IO;
    }
    return fffs_host_backend_from_verify_flash(backend, *flash);
}

static int mount_fs_with_policy(struct fffs *fs,
        const struct fffs_backend *backend, test_index_cache_t *index_cache,
        bool strict) {
    static uint8_t scratch[TEST_SCRATCH_SIZE];
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    static uint32_t alloc_map[TEST_ALLOC_MAP_WORDS];
#endif
    return fffs_mount(fs, backend, &(struct fffs_mount_options){
        .index_cache = index_cache,
        .index_cache_size = sizeof(index_cache[0]) * TEST_INDEX_CACHE_WORDS,
        .index_hash_table_size =
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
            TEST_INDEX_HASH_TABLE_SIZE,
#else
            FFFS_INDEX_HASH_TABLE_SIZE,
#endif
        .scratch = scratch,
        .scratch_size = sizeof(scratch),
        .strict = strict,
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
        .alloc_map = alloc_map,
        .alloc_map_words = TEST_ALLOC_MAP_WORDS,
#endif
    });
}

static int mount_fs(struct fffs *fs, const struct fffs_backend *backend,
        test_index_cache_t *index_cache) {
    return mount_fs_with_policy(fs, backend, index_cache, false);
}

static size_t test_max_file_data_size(const struct fffs *fs) {
    size_t raw = fs->sector_size - 10 - 64;
    return raw - (raw % fs->backend.program_granule);
}

static int test_mount_requires_scratch(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    uint8_t tiny_scratch[FFFS_MIN_SCRATCH_SIZE - 1u];

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));

    ASSERT_EQ_INT(FFFS_ERR_INVALID, fffs_mount(&fs, &backend,
                &(struct fffs_mount_options){
                    .index_cache = fs_index_heads,
                    .index_cache_size = sizeof(fs_index_heads),
                    .index_hash_table_size = TEST_INDEX_HASH_TABLE_SIZE,
                }));
    ASSERT_EQ_INT(FFFS_ERR_INVALID, fffs_mount(&fs, &backend,
                &(struct fffs_mount_options){
                    .index_cache = fs_index_heads,
                    .index_cache_size = sizeof(fs_index_heads),
                    .index_hash_table_size = TEST_INDEX_HASH_TABLE_SIZE,
                    .scratch = tiny_scratch,
                    .scratch_size = sizeof(tiny_scratch),
                }));

    ffsv_flash_destroy(flash);
    return 0;
}

static int write_chunks(struct fffs *fs, const char *name,
        const uint8_t *data, size_t size) {
    struct fffs_file file;
    ASSERT_OK(fffs_open(fs, &file, name,
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    for (size_t off = 0; off < size; off += 3) {
        size_t n = size - off < 3 ? size - off : 3;
        ASSERT_OK(fffs_write(&file, data + off, n));
    }
    return fffs_close(&file);
}

static int read_chunks(struct fffs *fs, const char *name,
        uint8_t *out, size_t capacity, size_t *out_size) {
    struct fffs_file file;
    struct fffs_stat st;
    size_t total = 0;
    ASSERT_OK(fffs_open(fs, &file, name, FFFS_O_RDONLY));
    ASSERT_OK(fffs_fstat(&file, &st));
    ASSERT_TRUE(st.size <= capacity);
    while (total < st.size) {
        size_t nread = 0;
        ASSERT_OK(fffs_read(&file, out + total, 2, &nread));
        ASSERT_TRUE(nread > 0);
        total += nread;
    }
    ASSERT_OK(fffs_close(&file));
    *out_size = total;
    return FFFS_OK;
}

static int count_dir(struct fffs *fs, const char *prefix, size_t *out_count) {
    struct fffs_dir dir;
    struct fffs_stat st;
    size_t count = 0;

    ASSERT_OK(fffs_dir_open(fs, &dir, prefix));
    while (fffs_dir_read(&dir, &st)) {
        ASSERT_TRUE(prefix == NULL ||
                strncmp(st.name, prefix, strlen(prefix)) == 0);
        count += 1;
    }
    ASSERT_OK(fffs_dir_status(&dir));
    ASSERT_OK(fffs_dir_close(&dir));
    *out_count = count;
    return FFFS_OK;
}

static int test_format_mount_write_read_remount(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint8_t alpha[] = {1, 2, 3, 4, 5};
    const uint8_t beta[] = {0xaa, 0xbb, 0xcc};
    uint8_t out[16] = {0};
    size_t out_size = 0;
    bool exists = false;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "alpha", alpha, sizeof(alpha)));
    ASSERT_OK(write_chunks(&fs, "beta", beta, sizeof(beta)));
    ASSERT_OK(write_chunks(&fs, "cfg/net", alpha, sizeof(alpha)));
    ASSERT_OK(write_chunks(&fs, "cfg/ui", beta, sizeof(beta)));
    ASSERT_TRUE(fs.alloc_cursor == fs.index_sectors);
    ASSERT_TRUE(fs.next_sector_serial == 2);

    struct fffs_stat entries[4];
    size_t count = 0;
    ASSERT_OK(fffs_list(&fs, entries, 4, &count));
    ASSERT_TRUE(count == 4);
    ASSERT_OK(count_dir(&fs, NULL, &count));
    ASSERT_TRUE(count == 4);
    ASSERT_OK(count_dir(&fs, "cfg/", &count));
    ASSERT_TRUE(count == 2);
    ASSERT_OK(count_dir(&fs, "missing/", &count));
    ASSERT_TRUE(count == 0);
    ASSERT_OK(fffs_exists(&fs, "alpha", &exists));
    ASSERT_TRUE(exists);
    ASSERT_OK(fffs_exists(&fs, "missing", &exists));
    ASSERT_TRUE(!exists);

    ASSERT_OK(read_chunks(&fs, "alpha", out, sizeof(out), &out_size));
    ASSERT_TRUE(out_size == sizeof(alpha));
    ASSERT_TRUE(memcmp(out, alpha, sizeof(alpha)) == 0);
    fffs_unmount(&fs);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_TRUE(remounted.alloc_cursor >= remounted.index_sectors);
    ASSERT_TRUE(remounted.next_sector_serial == 2);
    memset(out, 0, sizeof(out));
    ASSERT_OK(read_chunks(&remounted, "beta", out, sizeof(out), &out_size));
    ASSERT_TRUE(out_size == sizeof(beta));
    ASSERT_TRUE(memcmp(out, beta, sizeof(beta)) == 0);

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_partial_full_hint_does_not_invalidate_sector(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint8_t value[] = {0x31, 0x32, 0x33, 0x34};
    uint8_t out[8] = {0};
    size_t out_size = 0;
    struct fffs_inspect_summary summary;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "config", value, sizeof(value)));

    uint16_t slot = fffs_hash16("config");
    uint16_t head = 0;
    bool found = false;
    ASSERT_OK(fffs_index_head_for_slot(&fs, slot, &head, &found));
    ASSERT_TRUE(found);

    size_t granule = fs.backend.program_granule;
    ASSERT_TRUE(granule <= 4u);
    size_t state_off = (size_t)head * fs.sector_size +
        fs.sector_size - 10u + 5u;
    size_t prog_off = state_off - (state_off % granule);
    uint8_t partial[4] = {0xff, 0xff, 0xff, 0xff};
    partial[state_off - prog_off] = 0x5e;
    ASSERT_TRUE(ffsv_flash_image_byte(flash, state_off) == 0x7e);
    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash, prog_off,
                    partial, granule, FFSV_CALLSITE)));
    ASSERT_TRUE(ffsv_flash_image_byte(flash, state_off) == 0x5e);

    fffs_unmount(&fs);
    ASSERT_OK(fffs_inspect_check(&backend, &summary));
    ASSERT_TRUE(summary.data_sectors_corrupt == 0);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_OK(read_chunks(&remounted, "config", out, sizeof(out),
                &out_size));
    ASSERT_TRUE(out_size == sizeof(value));
    ASSERT_TRUE(memcmp(out, value, sizeof(value)) == 0);

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_allocator_repairs_partial_full_hint_after_scan(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint8_t value[] = {0x10, 0x11, 0x12, 0x13};

    ASSERT_OK(new_backend_with_size(&flash, &backend, 256 * 32));
    ASSERT_OK(fffs_format(&backend, &(struct fffs_format_options){
                .index_sectors = 2,
                .sector_size = FFFS_SECTOR_256,
            }));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    for (size_t i = 0; i < 4; i++) {
        char name[8];
        snprintf(name, sizeof(name), "f%03u", (unsigned)i);
        ASSERT_OK(write_chunks(&fs, name, value, sizeof(value)));
    }

    uint16_t slot = fffs_hash16("f000");
    uint16_t head = 0;
    bool found = false;
    ASSERT_OK(fffs_index_head_for_slot(&fs, slot, &head, &found));
    ASSERT_TRUE(found);

    size_t granule = fs.backend.program_granule;
    ASSERT_TRUE(granule <= 4u);
    size_t state_off = (size_t)head * fs.sector_size +
        fs.sector_size - 10u + 5u;
    size_t prog_off = state_off - (state_off % granule);
    uint8_t partial[4] = {0xff, 0xff, 0xff, 0xff};
    partial[state_off - prog_off] = 0x5e;
    ASSERT_TRUE(ffsv_flash_image_byte(flash, state_off) == 0x7e);
    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash, prog_off,
                    partial, granule, FFSV_CALLSITE)));
    ASSERT_TRUE(ffsv_flash_image_byte(flash, state_off) == 0x5e);

    ASSERT_OK(write_chunks(&fs, "f004", value, sizeof(value)));
    ASSERT_TRUE(ffsv_flash_image_byte(flash, state_off) == 0x5a);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_free_window_rejects_partially_erased_footer_without_scan(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    uint16_t data_off = 0;
    uint16_t record_off = 0;
    bool needs_footer = false;
    uint16_t md_records = 0;

    ASSERT_OK(new_backend_with_sector_size(&flash, &backend, 256, 16));
    ASSERT_OK(fffs_format(&backend, &(struct fffs_format_options){
                .index_sectors = 2,
                .sector_size = FFFS_SECTOR_256,
            }));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    uint16_t sector = fs.index_sectors;
    size_t corrupt_off = (size_t)(sector + 1u) * fs.sector_size - 4u;
    uint8_t tail[4] = {0xff, 0xff, 0x00, 0x00};
    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash, corrupt_off,
                    tail, sizeof(tail), FFSV_CALLSITE)));

    const struct ffsv_op_counts *before = ffsv_flash_counts(flash);
    uint64_t read_bytes_before = before[FFSV_OP_READ].bytes;
    ASSERT_EQ_INT(FFFS_ERR_NO_SPACE,
            fffs_find_sector_free_window(&fs, sector, 1, 0, true,
                &data_off, &record_off, &needs_footer, &md_records));
    const struct ffsv_op_counts *after = ffsv_flash_counts(flash);
    ASSERT_TRUE(after[FFSV_OP_READ].bytes - read_bytes_before ==
            FFFS_MD_PRELOAD_MAX);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_overwrite_delete_and_remount(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    const char *old_value = "old";
    const char *new_value = "new value";
    uint8_t out[32] = {0};
    size_t out_size = 0;
    struct fffs_stat st;
    struct fffs_inspect_summary summary;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "config", (const uint8_t *)old_value,
                strlen(old_value)));
    ASSERT_TRUE(fs.alloc_cursor == fs.index_sectors);
    ASSERT_TRUE(fs.next_sector_serial == 2);
    ASSERT_OK(write_chunks(&fs, "config", (const uint8_t *)new_value,
                strlen(new_value)));
    ASSERT_TRUE(fs.alloc_cursor == fs.index_sectors);
    ASSERT_TRUE(fs.next_sector_serial == 3);
    ASSERT_OK(fffs_stat(&fs, "config", &st));
    ASSERT_TRUE(st.size == strlen(new_value));
    ASSERT_OK(read_chunks(&fs, "config", out, sizeof(out), &out_size));
    ASSERT_TRUE(out_size == strlen(new_value));
    ASSERT_TRUE(memcmp(out, new_value, out_size) == 0);
    ASSERT_OK(fffs_delete_file(&fs, "config"));
    ASSERT_EQ_INT(FFFS_ERR_NOT_FOUND, fffs_stat(&fs, "config", &st));
    ASSERT_OK(fffs_inspect_check(&backend, &summary));
#if FFFS_LAZY_DELETE_TOMBSTONES
    ASSERT_TRUE(summary.md_tombstoned == 0);
#else
    ASSERT_TRUE(summary.md_tombstoned == 2);
#endif
    fffs_unmount(&fs);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_TRUE(remounted.alloc_cursor >= remounted.index_sectors);
    ASSERT_TRUE(remounted.next_sector_serial == 3);
    ASSERT_EQ_INT(FFFS_ERR_NOT_FOUND, fffs_stat(&remounted, "config", &st));

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_delete_tombstones_each_sector_in_contiguous_span(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const size_t large_size = 24u * 1024u;
    uint8_t *large = malloc(large_size);
    struct fffs_md_record root_record;
    size_t record_starts[16] = {0};
    uint16_t head;
    uint16_t slot;
    uint16_t span_len;
    struct fffs_stat st;
    struct fffs_inspect_summary summary;

    ASSERT_TRUE(large != NULL);
    for (size_t i = 0; i < large_size; i++) {
        large[i] = (uint8_t)(i * 17u + (i >> 4));
    }

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 32));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "large.bin", large, large_size));

    ASSERT_OK(fffs_open(&fs, &file, "large.bin", FFFS_O_RDONLY));
    ASSERT_OK(fffs_read_md_for_slot(&fs, file.head, file.slot, &root_record));
    ASSERT_TRUE(root_record.span_len > 1);
    ASSERT_TRUE(root_record.span_len <=
            sizeof(record_starts) / sizeof(record_starts[0]));
    head = file.head;
    slot = file.slot;
    span_len = root_record.span_len;
    ASSERT_OK(fffs_close(&file));

    for (uint16_t i = 0; i < span_len; i++) {
        struct fffs_md_record record;
        ASSERT_OK(fffs_read_md_for_slot(&fs, (uint16_t)(head + i), slot,
                    &record));
        record_starts[i] = record.record_start;
    }

    ASSERT_OK(fffs_delete_file(&fs, "large.bin"));
    ASSERT_EQ_INT(FFFS_ERR_NOT_FOUND, fffs_stat(&fs, "large.bin", &st));
    ASSERT_OK(fffs_inspect_check(&backend, &summary));
#if FFFS_LAZY_DELETE_TOMBSTONES
    ASSERT_TRUE(summary.md_tombstoned == 0);
    for (uint16_t i = 0; i < span_len; i++) {
        ASSERT_TRUE(ffsv_flash_image_byte(flash,
                    (size_t)(head + i) * fs.sector_size + record_starts[i]) ==
                TEST_MD_FLAGS_VALID);
    }
#else
    ASSERT_TRUE(summary.md_tombstoned == span_len);
    for (uint16_t i = 0; i < span_len; i++) {
        ASSERT_TRUE(ffsv_flash_image_byte(flash,
                    (size_t)(head + i) * fs.sector_size + record_starts[i]) ==
                TEST_MD_FLAGS_TOMBSTONED);
    }
#endif

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    free(large);
    return 0;
}

static int test_fsinfo_refresh_and_cached_accounting(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    struct fffs_fsinfo info;
    struct fffs_file writer;
    const char *alpha = "abc";
    const char *beta = "12345";
    const char *gamma = "payload";
    const char *alpha_new = "wxyz";

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(fffs_fsinfo(&fs, &info, FFFS_FSINFO_FAST));
    ASSERT_TRUE((info.valid_flags & FFFS_FSINFO_TOTAL_VALID) != 0);
    ASSERT_TRUE((info.valid_flags & FFFS_FSINFO_PENDING_VALID) != 0);
    ASSERT_TRUE((info.valid_flags & FFFS_FSINFO_COMMITTED_FILES_VALID) == 0);
    ASSERT_TRUE(info.total_bytes == (uint32_t)((fs.sector_count -
                    fs.index_sectors) * (fs.sector_size - 10)));
    ASSERT_TRUE(info.pending_file_count == 0);
    ASSERT_TRUE(info.pending_data_bytes == 0);

    ASSERT_OK(write_chunks(&fs, "alpha", (const uint8_t *)alpha,
                strlen(alpha)));
    ASSERT_OK(write_chunks(&fs, "beta", (const uint8_t *)beta,
                strlen(beta)));

    ASSERT_OK(fffs_fsinfo(&fs, &info, FFFS_FSINFO_FAST));
    ASSERT_TRUE((info.valid_flags & FFFS_FSINFO_COMMITTED_FILES_VALID) == 0);

    ASSERT_OK(fffs_fsinfo(&fs, &info, FFFS_FSINFO_REFRESH_IF_NEEDED));
    ASSERT_TRUE(info.committed_file_count == 2);
    ASSERT_TRUE(info.committed_data_bytes == strlen(alpha) + strlen(beta));

    ASSERT_OK(fffs_fsinfo(&fs, &info, FFFS_FSINFO_REFRESH_COMMITTED |
                FFFS_FSINFO_ESTIMATE_METADATA));
    ASSERT_TRUE((info.valid_flags & FFFS_FSINFO_COMMITTED_FILES_VALID) != 0);
    ASSERT_TRUE((info.valid_flags & FFFS_FSINFO_COMMITTED_BYTES_VALID) != 0);
    ASSERT_TRUE((info.valid_flags & FFFS_FSINFO_METADATA_ESTIMATE_VALID) != 0);
    ASSERT_TRUE(info.committed_file_count == 2);
    ASSERT_TRUE(info.committed_data_bytes == strlen(alpha) + strlen(beta));
    ASSERT_TRUE(info.estimated_metadata_bytes > 0);

    ASSERT_OK(fffs_open(&fs, &writer, "gamma",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_OK(fffs_write(&writer, gamma, strlen(gamma)));
    ASSERT_OK(fffs_fsinfo(&fs, &info, FFFS_FSINFO_FAST));
    ASSERT_TRUE(info.committed_file_count == 2);
    ASSERT_TRUE(info.committed_data_bytes == strlen(alpha) + strlen(beta));
    ASSERT_TRUE(info.pending_file_count == 1);
    ASSERT_TRUE(info.pending_data_bytes == strlen(gamma));
    ASSERT_OK(fffs_close(&writer));

    ASSERT_OK(fffs_fsinfo(&fs, &info, FFFS_FSINFO_FAST));
    ASSERT_TRUE(info.committed_file_count == 3);
    ASSERT_TRUE(info.committed_data_bytes ==
            strlen(alpha) + strlen(beta) + strlen(gamma));
    ASSERT_TRUE(info.pending_file_count == 0);
    ASSERT_TRUE(info.pending_data_bytes == 0);

    ASSERT_OK(write_chunks(&fs, "alpha", (const uint8_t *)alpha_new,
                strlen(alpha_new)));
    ASSERT_OK(fffs_fsinfo(&fs, &info, FFFS_FSINFO_FAST));
#if FFFS_LAZY_DELETE_TOMBSTONES
    ASSERT_TRUE((info.valid_flags & FFFS_FSINFO_COMMITTED_FILES_VALID) == 0);
#else
    ASSERT_TRUE(info.committed_file_count == 3);
    ASSERT_TRUE(info.committed_data_bytes ==
            strlen(alpha_new) + strlen(beta) + strlen(gamma));
#endif

    ASSERT_OK(fffs_fsinfo(&fs, &info, FFFS_FSINFO_REFRESH_COMMITTED));
    ASSERT_TRUE(info.committed_file_count == 3);
    ASSERT_TRUE(info.committed_data_bytes ==
            strlen(alpha_new) + strlen(beta) + strlen(gamma));

    ASSERT_OK(fffs_delete_file(&fs, "beta"));
    ASSERT_OK(fffs_fsinfo(&fs, &info, FFFS_FSINFO_FAST));
#if FFFS_LAZY_DELETE_TOMBSTONES
    ASSERT_TRUE((info.valid_flags & FFFS_FSINFO_COMMITTED_FILES_VALID) == 0);
#else
    ASSERT_TRUE(info.committed_file_count == 2);
    ASSERT_TRUE(info.committed_data_bytes ==
            strlen(alpha_new) + strlen(gamma));
#endif

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_reserved_hash_slots_are_skipped(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint8_t zero_hash[] = {0x11};
    const uint8_t erased_hash[] = {0x22};
    uint8_t out[4] = {0};
    size_t out_size = 0;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "xer", zero_hash, sizeof(zero_hash)));
    ASSERT_OK(write_chunks(&fs, "x1hk", erased_hash, sizeof(erased_hash)));

    ASSERT_OK(read_chunks(&fs, "xer", out, sizeof(out), &out_size));
    ASSERT_TRUE(out_size == sizeof(zero_hash));
    ASSERT_TRUE(memcmp(out, zero_hash, sizeof(zero_hash)) == 0);
    ASSERT_OK(read_chunks(&fs, "x1hk", out, sizeof(out), &out_size));
    ASSERT_TRUE(out_size == sizeof(erased_hash));
    ASSERT_TRUE(memcmp(out, erased_hash, sizeof(erased_hash)) == 0);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_replay_skips_reused_stale_index_heads(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    const char *old_name = "stale-a";
    const char *new_name = "fresh-b";
    const uint8_t old_data[] = "old";
    const uint8_t new_data[] = "new";
    struct fffs_stat entries[4];
    struct fffs_stat st;
    size_t count = 0;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(write_chunks(&fs, old_name, old_data, sizeof(old_data) - 1));
    uint16_t reused = fs.index_sectors;
    ASSERT_OK(fffs_delete_file(&fs, old_name));

    ASSERT_OK(flash_to_fs(backend.erase(backend.ctx,
                    (size_t)reused * fs.sector_size, fs.sector_size)));
    fs.alloc_cursor = reused;
    ASSERT_OK(write_chunks(&fs, new_name, new_data, sizeof(new_data) - 1));
    fffs_unmount(&fs);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_EQ_INT(FFFS_ERR_NOT_FOUND, fffs_stat(&remounted, old_name, &st));
    ASSERT_OK(fffs_stat(&remounted, new_name, &st));
    ASSERT_TRUE(st.size == sizeof(new_data) - 1);
    ASSERT_OK(fffs_list(&remounted, entries,
                sizeof(entries) / sizeof(entries[0]), &count));
    ASSERT_TRUE(count == 1);
    ASSERT_TRUE(strcmp(entries[0].name, new_name) == 0);

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_non_strict_mount_skips_invalid_active_index_tail(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    struct fffs strict_fs;
    struct fffs_file file;
    struct fffs_file next_file;
    struct fffs_stat st;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t strict_index_heads[TEST_INDEX_CACHE_WORDS];
    const char *name = "tail";
    const uint8_t data[] = "committed";

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(fffs_open(&fs, &file, name,
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_OK(fffs_write(&file, data, sizeof(data) - 1));
    uint16_t head = file.head;
    ASSERT_OK(fffs_close(&file));
    size_t torn_offset = fs.next_index_offset;

    uint16_t bad_slot = fffs_hash16(name) ^ 0x1111u;
    if (bad_slot == 0 || bad_slot == UINT16_MAX) {
        bad_slot ^= 0x2222u;
    }
    uint8_t torn_record[4] = {
        (uint8_t)(bad_slot & 0xffu),
        (uint8_t)(bad_slot >> 8),
        (uint8_t)(head & 0xffu),
        (uint8_t)(head >> 8),
    };
    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash, torn_offset,
                    torn_record, sizeof(torn_record), FFSV_CALLSITE)));
    fffs_unmount(&fs);

    ASSERT_EQ_INT(FFFS_ERR_CORRUPT,
            mount_fs_with_policy(&strict_fs, &backend, strict_index_heads,
                true));
    for (size_t i = 0; i < sizeof(torn_record); i++) {
        ASSERT_TRUE(ffsv_flash_image_byte(flash, torn_offset + i) ==
                torn_record[i]);
    }

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    for (size_t i = 0; i < sizeof(torn_record); i++) {
        ASSERT_TRUE(ffsv_flash_image_byte(flash, torn_offset + i) == 0x00);
    }
    ASSERT_OK(fffs_stat(&remounted, name, &st));
    ASSERT_TRUE(st.size == sizeof(data) - 1);
    ASSERT_OK(fffs_open(&remounted, &next_file, "after-tail",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_OK(fffs_write(&next_file, data, sizeof(data) - 1));
    ASSERT_OK(fffs_close(&next_file));
    ASSERT_OK(fffs_stat(&remounted, "after-tail", &st));
    ASSERT_TRUE(st.size == sizeof(data) - 1);
    fffs_unmount(&remounted);

    ffsv_flash_destroy(flash);
    return 0;
}

static int test_non_strict_mount_does_not_clobber_nonterminal_index_record(
        void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    const char *name = "not-tail";
    const uint8_t data[] = "committed";

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(fffs_open(&fs, &file, name,
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_OK(fffs_write(&file, data, sizeof(data) - 1));
    ASSERT_OK(fffs_close(&file));
    size_t bad_offset = fs.next_index_offset;

    uint8_t bad_record[4] = {0x34, 0x12, 0x01, 0x00};
    uint8_t following_record[4] = {0x35, 0x12, 0x00, 0x00};
    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash, bad_offset,
                    bad_record, sizeof(bad_record), FFSV_CALLSITE)));
    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash, bad_offset + 4,
                    following_record, sizeof(following_record),
                    FFSV_CALLSITE)));
    fffs_unmount(&fs);

    ASSERT_EQ_INT(FFFS_ERR_CORRUPT,
            mount_fs(&remounted, &backend, remount_index_heads));
    for (size_t i = 0; i < sizeof(bad_record); i++) {
        ASSERT_TRUE(ffsv_flash_image_byte(flash, bad_offset + i) ==
                bad_record[i]);
        ASSERT_TRUE(ffsv_flash_image_byte(flash,
                    bad_offset + sizeof(bad_record) + i) ==
                following_record[i]);
    }

    ffsv_flash_destroy(flash);
    return 0;
}

static int test_gc_reclaims_unindexed_orphan_sector(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    uint8_t footer_program[12] = {
        0xff, 0xff,
        0x7b, 0x00, 0x00, 0x00,
        0x01, 0x7e,
        'F', 'F', 'S', 'D',
    };
    uint8_t check[10];
    enum fffs_gc_action action;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash,
                    10 * FFFS_DEFAULT_SECTOR_SIZE +
                    FFFS_DEFAULT_SECTOR_SIZE - sizeof(footer_program),
                    footer_program, sizeof(footer_program), FFSV_CALLSITE)));
    fs.gc_cursor = 10;
    do {
        ASSERT_OK(fffs_gc_step(&fs, &action));
    } while (action == FFFS_GC_SCANNED);
    ASSERT_EQ_INT(FFFS_GC_TOMBSTONED, action);
    ASSERT_OK(flash_to_fs(ffsv_flash_read(flash,
                    10 * FFFS_DEFAULT_SECTOR_SIZE +
                    FFFS_DEFAULT_SECTOR_SIZE - sizeof(check),
                    check, sizeof(check), FFSV_CALLSITE)));
    ASSERT_TRUE(check[5] == 0x3c);
    ASSERT_OK(fffs_gc_step(&fs, &action));
    ASSERT_TRUE(action == FFFS_GC_ERASED);
    ASSERT_OK(flash_to_fs(ffsv_flash_read(flash,
                    10 * FFFS_DEFAULT_SECTOR_SIZE +
                    FFFS_DEFAULT_SECTOR_SIZE - sizeof(check),
                    check, sizeof(check), FFSV_CALLSITE)));
    for (size_t i = 0; i < sizeof(check); i++) {
        ASSERT_TRUE(check[i] == 0xff);
    }

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_gc_erases_dirty_sector_with_erased_footer(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    uint8_t data[16];
    enum fffs_gc_action action;

    memset(data, 0xa5, sizeof(data));
    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash,
                    10 * FFFS_DEFAULT_SECTOR_SIZE, data, sizeof(data),
                    FFSV_CALLSITE)));
    ASSERT_TRUE(!ffsv_flash_image_span_is_erased(flash,
                10 * FFFS_DEFAULT_SECTOR_SIZE, FFFS_DEFAULT_SECTOR_SIZE));
    fs.gc_cursor = 10;
    ASSERT_OK(fffs_gc_step(&fs, &action));
    ASSERT_TRUE(action == FFFS_GC_ERASED);
    ASSERT_TRUE(ffsv_flash_image_span_is_erased(flash,
                10 * FFFS_DEFAULT_SECTOR_SIZE, FFFS_DEFAULT_SECTOR_SIZE));

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_gc_tombstones_sector_with_invalid_unknown_md(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint16_t sector = 10;
    size_t base = (size_t)sector * FFFS_DEFAULT_SECTOR_SIZE;
    size_t footer_off = FFFS_DEFAULT_SECTOR_SIZE - TEST_SECTOR_FOOTER_SIZE;
    size_t record_off = footer_off - TEST_MD_FILE_RECORD_SIZE;
    uint8_t footer[TEST_SECTOR_FOOTER_SIZE] = {
        0x7b, 0x00, 0x00, 0x00,
        TEST_SECTOR_TYPE_FILE,
        TEST_SECTOR_FLAGS_VALID,
        'F', 'F', 'S', 'D',
    };
    uint8_t type = TEST_MD_TYPE_UNKNOWN;
    enum fffs_gc_action action;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(flash_to_fs(ffsv_flash_corrupt(flash, base + footer_off,
                    footer, sizeof(footer), FFSV_CALLSITE)));
    ASSERT_OK(flash_to_fs(ffsv_flash_corrupt(flash,
                    base + record_off + TEST_MD_FILE_RECORD_SIZE - 1u,
                    &type, sizeof(type), FFSV_CALLSITE)));

    fs.gc_cursor = sector;
    ASSERT_OK(fffs_gc_step(&fs, &action));
    ASSERT_EQ_INT(FFFS_GC_TOMBSTONED, action);
    ASSERT_TRUE(ffsv_flash_image_byte(flash, base + footer_off + 5u) ==
            TEST_MD_FLAGS_TOMBSTONED);
    ASSERT_TRUE(ffsv_flash_image_byte(flash, base + record_off) == 0xff);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_gc_tombstones_sector_with_reachable_invalid_metadata_normally(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint8_t data[] = "committed";
    uint8_t bad_span[2] = {0x00, 0x00};
    uint16_t head;
    uint16_t record_off;
    enum fffs_gc_action action;
    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(fffs_open(&fs, &file, "reachable-bad-md",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_OK(fffs_write(&file, data, sizeof(data) - 1));
    head = file.head;
    record_off = file.current_metadata_offset;
    ASSERT_OK(fffs_close(&file));

    ASSERT_OK(flash_to_fs(ffsv_flash_corrupt(flash,
                    (size_t)head * fs.sector_size + record_off + 5u,
                    bad_span, sizeof(bad_span), FFSV_CALLSITE)));
    ASSERT_TRUE(ffsv_flash_image_byte(flash,
                (size_t)head * fs.sector_size + record_off) ==
            TEST_MD_FLAGS_VALID);

    fs.gc_cursor = head;
#if FFFS_GC_PARANOID_REACHABILITY
    ASSERT_EQ_INT(FFFS_ERR_CORRUPT, fffs_gc_step(&fs, &action));
    ASSERT_TRUE(ffsv_flash_image_byte(flash,
                (size_t)head * fs.sector_size + record_off) ==
            TEST_MD_FLAGS_VALID);
#else
    ASSERT_OK(fffs_gc_step(&fs, &action));
    ASSERT_EQ_INT(FFFS_GC_TOMBSTONED, action);
    ASSERT_TRUE(ffsv_flash_image_byte(flash,
                (size_t)head * fs.sector_size + record_off) ==
            TEST_MD_FLAGS_VALID);
    ASSERT_TRUE(ffsv_flash_image_byte(flash,
                (size_t)head * fs.sector_size + fs.sector_size -
                    TEST_SECTOR_FOOTER_SIZE + 5u) ==
            TEST_MD_FLAGS_TOMBSTONED);
#endif

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_gc_skips_open_writer_dirty_root_sector(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    uint8_t data[FFFS_FILE_CACHE_SIZE];
    enum fffs_gc_action action;
    memset(data, 0x5a, sizeof(data));
    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(fffs_open(&fs, &file, "stream",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_OK(fffs_write(&file, data, sizeof(data)));
    ASSERT_TRUE(!ffsv_flash_image_span_is_erased(flash,
                (size_t)file.head * fs.sector_size, fs.sector_size));

    fs.gc_cursor = file.head;
    ASSERT_OK(fffs_gc_step(&fs, &action));
    ASSERT_TRUE(!ffsv_flash_image_span_is_erased(flash,
                (size_t)file.head * fs.sector_size, fs.sector_size));

    ASSERT_OK(fffs_close(&file));
    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_gc_skips_multiple_open_writer_dirty_sectors(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file first;
    struct fffs_file second;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    uint8_t data[FFFS_FILE_CACHE_SIZE];
    enum fffs_gc_action action;
    memset(data, 0x33, sizeof(data));
    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(fffs_open(&fs, &first, "first",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_OK(fffs_write(&first, data, sizeof(data)));
    ASSERT_OK(fffs_open(&fs, &second, "second",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_OK(fffs_write(&second, data, sizeof(data)));

    fs.gc_cursor = first.head;
    ASSERT_OK(fffs_gc_step(&fs, &action));
    ASSERT_TRUE(action == FFFS_GC_SCANNED);
    ASSERT_TRUE(!ffsv_flash_image_span_is_erased(flash,
                (size_t)first.head * fs.sector_size, fs.sector_size));

    fs.gc_cursor = second.head;
    ASSERT_OK(fffs_gc_step(&fs, &action));
    ASSERT_TRUE(action == FFFS_GC_SCANNED);
    ASSERT_TRUE(!ffsv_flash_image_span_is_erased(flash,
                (size_t)second.head * fs.sector_size, fs.sector_size));

    ASSERT_OK(fffs_close(&second));
    ASSERT_OK(fffs_close(&first));
    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_gc_skips_open_writer_root_and_current_extents(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    uint8_t data[FFFS_FILE_CACHE_SIZE];
    enum fffs_gc_action action;
    memset(data, 0xc3, sizeof(data));
    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(fffs_open(&fs, &file, "large",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    for (size_t off = 0; off < test_max_file_data_size(&fs) +
            sizeof(data); off += sizeof(data)) {
        size_t remaining = test_max_file_data_size(&fs) + sizeof(data) - off;
        size_t n = remaining < sizeof(data) ? remaining : sizeof(data);
        ASSERT_OK(fffs_write(&file, data, n));
    }
    ASSERT_TRUE(file.current != file.head);

    fs.gc_cursor = file.head;
    ASSERT_OK(fffs_gc_step(&fs, &action));
    ASSERT_TRUE(!ffsv_flash_image_span_is_erased(flash,
                (size_t)file.head * fs.sector_size, fs.sector_size));

    /*
     * Starting GC at the writer's current extent must not disturb it. Unlike
     * the sealed head/middle extents, the current sector is legitimately blank
     * on flash (its data is still buffered in RAM), so an erasure check does not
     * apply here -- exercising the step without error is the observable that the
     * in-flight sector was skipped rather than reclaimed.
     */
    fs.gc_cursor = file.current;
    ASSERT_OK(fffs_gc_step(&fs, &action));
    ASSERT_TRUE(file.current != 0);

    ASSERT_OK(fffs_close(&file));
    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_gc_skips_open_writer_middle_extent(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    uint8_t data[FFFS_FILE_CACHE_SIZE];
    enum fffs_gc_action action;
    uint16_t middle;

    memset(data, 0x9c, sizeof(data));
    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(fffs_open(&fs, &file, "huge",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    for (size_t off = 0; off < test_max_file_data_size(&fs) * 2 +
            sizeof(data); off += sizeof(data)) {
        size_t remaining = test_max_file_data_size(&fs) * 2 +
            sizeof(data) - off;
        size_t n = remaining < sizeof(data) ? remaining : sizeof(data);
        ASSERT_OK(fffs_write(&file, data, n));
    }
    ASSERT_TRUE(file.current - file.span_head + 1u > 1);
    ASSERT_TRUE(file.current != file.head);
    middle = (uint16_t)(file.head + 1u);

    fs.gc_cursor = middle;
    ASSERT_OK(fffs_gc_step(&fs, &action));
    ASSERT_TRUE(!ffsv_flash_image_span_is_erased(flash,
                (size_t)middle * fs.sector_size, fs.sector_size));

    ASSERT_OK(fffs_close(&file));
    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_gc_skips_open_writer_deep_middle_extent(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    uint8_t data[FFFS_FILE_CACHE_SIZE];
    enum fffs_gc_action action;
    uint16_t second_middle;

    memset(data, 0x72, sizeof(data));
    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(fffs_open(&fs, &file, "deeper",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    for (size_t off = 0; off < test_max_file_data_size(&fs) * 3 +
            sizeof(data); off += sizeof(data)) {
        size_t remaining = test_max_file_data_size(&fs) * 3 +
            sizeof(data) - off;
        size_t n = remaining < sizeof(data) ? remaining : sizeof(data);
        ASSERT_OK(fffs_write(&file, data, n));
    }
    ASSERT_TRUE(file.current - file.span_head + 1u > 2);
    ASSERT_TRUE(file.current != file.head);
    second_middle = (uint16_t)(file.head + 2u);
    ASSERT_TRUE(second_middle != file.current);

    fs.gc_cursor = second_middle;
    ASSERT_OK(fffs_gc_step(&fs, &action));
    ASSERT_TRUE(!ffsv_flash_image_span_is_erased(flash,
                (size_t)second_middle * fs.sector_size, fs.sector_size));

    ASSERT_OK(fffs_close(&file));
    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_gc_reclaims_failed_open_writer_after_remount(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    uint8_t data[FFFS_FILE_CACHE_SIZE];
    enum fffs_gc_action action;
    uint16_t head;

    memset(data, 0xe1, sizeof(data));
    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(fffs_open(&fs, &file, "partial",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    head = file.head;
    inject_next(flash, FFSV_OP_PROGRAM, FFSV_FAIL_MIDDLE, 16);
    ASSERT_EQ_INT(FFFS_ERR_IO, fffs_write(&file, data, sizeof(data)));
    ASSERT_TRUE(!ffsv_flash_image_span_is_erased(flash,
                (size_t)head * fs.sector_size, fs.sector_size));

    fs.gc_cursor = head;
    ASSERT_OK(fffs_gc_step(&fs, &action));
    ASSERT_TRUE(!ffsv_flash_image_span_is_erased(flash,
                (size_t)head * fs.sector_size, fs.sector_size));

    fffs_unmount(&fs);
    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    remounted.gc_cursor = head;
    ASSERT_OK(fffs_gc_step(&remounted, &action));
    ASSERT_TRUE(action == FFFS_GC_ERASED);
    ASSERT_TRUE(ffsv_flash_image_span_is_erased(flash,
                (size_t)head * remounted.sector_size,
                remounted.sector_size));

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_gc_reclaims_obsolete_index_history(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    const char *old_value = "old";
    const char *new_value = "new";
    struct fffs_stat st;
    enum fffs_gc_action action;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "config", (const uint8_t *)old_value,
                strlen(old_value)));
    ASSERT_OK(write_chunks(&fs, "config", (const uint8_t *)new_value,
                strlen(new_value)));
    fs.gc_cursor = fs.index_sectors;
#if FFFS_LAZY_DELETE_TOMBSTONES
    do {
        ASSERT_OK(fffs_gc_step(&fs, &action));
    } while (action == FFFS_GC_SCANNED);
    ASSERT_EQ_INT(FFFS_GC_TOMBSTONED, action);
#endif
    do {
        ASSERT_OK(fffs_gc_step(&fs, &action));
    } while (action == FFFS_GC_SCANNED);
    ASSERT_EQ_INT(FFFS_GC_TOMBSTONED, action);
    ASSERT_OK(fffs_gc_step(&fs, &action));
    ASSERT_TRUE(action == FFFS_GC_ERASED);
    fffs_unmount(&fs);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_OK(fffs_stat(&remounted, "config", &st));
    ASSERT_TRUE(st.size == strlen(new_value));

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_alloc_failure_runs_gc_to_free_sector(void) {
#if !FFFS_GC_ON_ALLOC_FAILURE
    return 0;
#else
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint8_t value[] = {1, 2, 3, 4};
    const uint8_t replacement[] = {5, 6, 7, 8};
    struct fffs_stat st;

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 6));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(write_chunks(&fs, "f000", value, sizeof(value)));
    ASSERT_OK(write_chunks(&fs, "f001", value, sizeof(value)));
    ASSERT_OK(write_chunks(&fs, "f002", value, sizeof(value)));
    ASSERT_OK(write_chunks(&fs, "f003", value, sizeof(value)));
    ASSERT_OK(fffs_delete_file(&fs, "f001"));

    ASSERT_OK(write_chunks(&fs, "f004", replacement, sizeof(replacement)));
    ASSERT_OK(fffs_stat(&fs, "f004", &st));
    ASSERT_TRUE(st.size == sizeof(replacement));

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
#endif
}

static int test_alloc_reservation_skips_other_open_writer(void) {
#if FFFS_ALLOC_RESERVE_SECTORS <= 0
    return 0;
#else
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file first;
    struct fffs_file second;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];

    ASSERT_OK(new_backend_with_size(&flash, &backend,
                4096 * TEST_RESERVE_PRESSURE_BACKEND_SECTORS));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(fffs_open(&fs, &first, "first",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_TRUE(first.reserve_count > 0);
    uint16_t reserved = first.reserve_first;

    ASSERT_OK(fffs_open(&fs, &second, "second",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_TRUE(second.head != reserved);

    ASSERT_OK(fffs_close(&second));
    ASSERT_OK(fffs_close(&first));
    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
#endif
}

static int test_alloc_reservation_released_on_close(void) {
#if FFFS_ALLOC_RESERVE_SECTORS <= 0
    return 0;
#else
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file first;
    struct fffs_file second;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];

    ASSERT_OK(new_backend_with_size(&flash, &backend,
                4096 * TEST_RESERVE_PRESSURE_BACKEND_SECTORS));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(fffs_open(&fs, &first, "first",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_TRUE(first.reserve_count > 0);
    uint16_t reserved = first.reserve_first;
    uint16_t first_head = first.head;
    ASSERT_OK(fffs_close(&first));

    ASSERT_OK(fffs_open(&fs, &second, "second",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_TRUE(second.head == first_head || second.head == reserved);

    ASSERT_OK(fffs_close(&second));
    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
#endif
}

static int test_alloc_uses_owner_reservation_for_next_extent(void) {
#if FFFS_ALLOC_RESERVE_SECTORS <= 0
    return 0;
#else
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static uint8_t payload[FFFS_DEFAULT_SECTOR_SIZE + 1];

    ASSERT_OK(new_backend_with_size(&flash, &backend,
                4096 * TEST_RESERVE_BACKEND_SECTORS));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    memset(payload, 0x71, sizeof(payload));
    ASSERT_OK(fffs_open(&fs, &file, "large",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_TRUE(file.reserve_count > 0);
    size_t first_capacity = file.current_metadata_offset -
        file.data_offset - file.root_payload_offset;

    ASSERT_OK(fffs_write(&file, payload, first_capacity + 1));
    ASSERT_TRUE(file.current != file.head);

    ASSERT_OK(fffs_close(&file));
    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
#endif
}

static int test_alloc_skips_invalid_reserved_candidate(void) {
#if FFFS_ALLOC_RESERVE_SECTORS <= 0
    return 0;
#else
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static uint8_t payload[FFFS_DEFAULT_SECTOR_SIZE + 1];
    uint8_t dirty[FFFS_FILE_CACHE_SIZE];

    ASSERT_OK(new_backend_with_size(&flash, &backend,
                4096 * TEST_RESERVE_BACKEND_SECTORS));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    memset(payload, 0x29, sizeof(payload));
    memset(dirty, 0xa7, sizeof(dirty));
    ASSERT_OK(fffs_open(&fs, &file, "large",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_TRUE(file.reserve_count > 0);
    uint16_t reserved = file.reserve_first;
    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash,
                    (size_t)reserved * fs.sector_size, dirty, sizeof(dirty),
                    FFSV_CALLSITE)));

    ASSERT_OK(fffs_write(&file, payload,
                test_max_file_data_size(&fs) + 1));
    ASSERT_TRUE(file.current != reserved);

    ASSERT_OK(fffs_close(&file));
    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
#endif
}

static int test_alloc_trims_other_reservations_under_pressure(void) {
#if FFFS_ALLOC_RESERVE_SECTORS <= 1 || FFFS_COMPACTION_RESERVE_SECTORS > 0
    return 0;
#else
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file first;
    struct fffs_file second;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 7));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(fffs_open(&fs, &first, "first",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    uint16_t first_count = first.reserve_count;
    ASSERT_TRUE(first_count > 1);

    ASSERT_OK(fffs_open(&fs, &second, "second",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_TRUE(first.reserve_count == (first_count >> 1u));

    ASSERT_OK(fffs_close(&second));
    ASSERT_OK(fffs_close(&first));
    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
#endif
}

static int test_alloc_revokes_other_reservation_under_pressure(void) {
#if FFFS_ALLOC_RESERVE_SECTORS <= 0 || FFFS_COMPACTION_RESERVE_SECTORS > 0
    return 0;
#else
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file first;
    struct fffs_file second;
    struct fffs_file third;
    struct fffs_file fourth;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 6));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(fffs_open(&fs, &first, "first",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_TRUE(first.reserve_count > 0);
    ASSERT_OK(fffs_open(&fs, &second, "second",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_OK(fffs_open(&fs, &third, "third",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_OK(fffs_open(&fs, &fourth, "fourth",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_TRUE(first.reserve_count == 0);

    ASSERT_OK(fffs_close(&fourth));
    ASSERT_OK(fffs_close(&third));
    ASSERT_OK(fffs_close(&second));
    ASSERT_OK(fffs_close(&first));
    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
#endif
}

static int test_full_alloc_map_mount_requires_storage(void) {
#if FFFS_ALLOC_MAP_MODE != FFFS_ALLOC_MAP_FULL_BITMAP
    return 0;
#else
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static uint8_t scratch[TEST_SCRATCH_SIZE];
    uint32_t tiny_map[1];
    uint32_t ok_map[TEST_ALLOC_MAP_WORDS];

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 64));
    ASSERT_OK(fffs_format(&backend, NULL));

    ASSERT_EQ_INT(FFFS_ERR_INVALID, fffs_mount(&fs, &backend,
                &(struct fffs_mount_options){
                    .index_cache = fs_index_heads,
                    .index_cache_size = sizeof(fs_index_heads),
                    .index_hash_table_size = TEST_INDEX_HASH_TABLE_SIZE,
                    .scratch = scratch,
                    .scratch_size = sizeof(scratch),
                }));
    ASSERT_EQ_INT(FFFS_ERR_INVALID, fffs_mount(&fs, &backend,
                &(struct fffs_mount_options){
                    .index_cache = fs_index_heads,
                    .index_cache_size = sizeof(fs_index_heads),
                    .index_hash_table_size = TEST_INDEX_HASH_TABLE_SIZE,
                    .scratch = scratch,
                    .scratch_size = sizeof(scratch),
                    .alloc_map = tiny_map,
                    .alloc_map_words = sizeof(tiny_map) /
                        sizeof(tiny_map[0]),
                }));
    ASSERT_OK(fffs_mount(&fs, &backend, &(struct fffs_mount_options){
                    .index_cache = fs_index_heads,
                    .index_cache_size = sizeof(fs_index_heads),
                    .index_hash_table_size = TEST_INDEX_HASH_TABLE_SIZE,
                    .scratch = scratch,
                    .scratch_size = sizeof(scratch),
                    .alloc_map = ok_map,
                    .alloc_map_words = sizeof(ok_map) / sizeof(ok_map[0]),
                }));

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
#endif
}

static int test_streaming_write_forces_gc_without_reclaiming_self(void) {
#if !FFFS_GC_ON_ALLOC_FAILURE
    return 0;
#else
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint8_t value[] = {1, 2, 3, 4};
    static uint8_t payload[FFFS_DEFAULT_SECTOR_SIZE * 4];
    static uint8_t readback[FFFS_DEFAULT_SECTOR_SIZE * 4];
    size_t payload_size;
    size_t out_size = 0;
    struct fffs_file file;

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 10));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(write_chunks(&fs, "f000", value, sizeof(value)));
    ASSERT_OK(write_chunks(&fs, "f001", value, sizeof(value)));
    ASSERT_OK(write_chunks(&fs, "f002", value, sizeof(value)));
    ASSERT_OK(write_chunks(&fs, "f003", value, sizeof(value)));
    ASSERT_OK(write_chunks(&fs, "f004", value, sizeof(value)));
    ASSERT_OK(write_chunks(&fs, "f005", value, sizeof(value)));
    ASSERT_OK(write_chunks(&fs, "f006", value, sizeof(value)));
    ASSERT_OK(fffs_delete_file(&fs, "f001"));
    ASSERT_OK(fffs_delete_file(&fs, "f003"));
    ASSERT_OK(fffs_delete_file(&fs, "f005"));
    ASSERT_OK(fffs_delete_file(&fs, "f006"));

    payload_size = test_max_file_data_size(&fs) * 3 +
        FFFS_FILE_CACHE_SIZE;
    ASSERT_TRUE(payload_size <= sizeof(payload));
    for (size_t i = 0; i < payload_size; i++) {
        payload[i] = (uint8_t)(i * 31u + 7u);
    }

    const struct ffsv_op_counts *before = ffsv_flash_counts(flash);
    uint64_t erase_calls_before = before[FFSV_OP_ERASE].calls;
    ASSERT_OK(fffs_open(&fs, &file, "stream-big",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    fs.gc_cursor = file.head;
    for (size_t off = 0; off < payload_size; off += 137) {
        size_t n = payload_size - off < 137 ? payload_size - off : 137;
        ASSERT_OK(fffs_write(&file, payload + off, n));
    }
    ASSERT_OK(fffs_close(&file));

    const struct ffsv_op_counts *after = ffsv_flash_counts(flash);
    ASSERT_TRUE(after[FFSV_OP_ERASE].calls >= erase_calls_before);
    ASSERT_OK(read_chunks(&fs, "stream-big", readback, sizeof(readback),
                &out_size));
    ASSERT_TRUE(out_size == payload_size);
    ASSERT_TRUE(memcmp(readback, payload, payload_size) == 0);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
#endif
}

static int test_streaming_write_fails_after_gc_exhausts_reclaimable_space(void) {
#if !FFFS_GC_ON_ALLOC_FAILURE
    return 0;
#else
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint8_t value[] = {1, 2, 3, 4};
    static uint8_t payload[FFFS_DEFAULT_SECTOR_SIZE * 6];
    static uint8_t readback[FFFS_DEFAULT_SECTOR_SIZE * 6];
    size_t payload_size;
    size_t out_size = 0;
    struct fffs_file file;
    uint8_t extra = 0xaa;

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 10));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(write_chunks(&fs, "f000", value, sizeof(value)));
    ASSERT_OK(write_chunks(&fs, "f001", value, sizeof(value)));
    ASSERT_OK(write_chunks(&fs, "f002", value, sizeof(value)));
    ASSERT_OK(write_chunks(&fs, "f003", value, sizeof(value)));
    ASSERT_OK(write_chunks(&fs, "f004", value, sizeof(value)));
    ASSERT_OK(write_chunks(&fs, "f005", value, sizeof(value)));
    ASSERT_OK(write_chunks(&fs, "f006", value, sizeof(value)));
    ASSERT_OK(fffs_delete_file(&fs, "f001"));
    ASSERT_OK(fffs_delete_file(&fs, "f003"));
    ASSERT_OK(fffs_delete_file(&fs, "f005"));
    ASSERT_OK(fffs_delete_file(&fs, "f006"));

    payload_size = test_max_file_data_size(&fs) * 5;
    ASSERT_TRUE(payload_size <= sizeof(payload));
    for (size_t i = 0; i < payload_size; i++) {
        payload[i] = (uint8_t)(i * 17u + 19u);
    }

    const struct ffsv_op_counts *before = ffsv_flash_counts(flash);
    uint64_t erase_calls_before = before[FFSV_OP_ERASE].calls;
    ASSERT_OK(fffs_open(&fs, &file, "stream-full",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    fs.gc_cursor = file.head;
    for (size_t off = 0; off < payload_size; off += 251) {
        size_t n = payload_size - off < 251 ? payload_size - off : 251;
        ASSERT_OK(fffs_write(&file, payload + off, n));
    }
    ASSERT_OK(fffs_write(&file, &extra, sizeof(extra)));
    const struct ffsv_op_counts *after_fail = ffsv_flash_counts(flash);
    ASSERT_TRUE(after_fail[FFSV_OP_ERASE].calls >= erase_calls_before);

    ASSERT_OK(fffs_close(&file));
    ASSERT_OK(read_chunks(&fs, "stream-full", readback, sizeof(readback),
                &out_size));
    ASSERT_TRUE(out_size == payload_size + sizeof(extra));
    ASSERT_TRUE(memcmp(readback, payload, payload_size) == 0);
    ASSERT_TRUE(readback[payload_size] == extra);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
#endif
}

static int test_replay_evicts_stale_hash_collision_head(void) {
#if FFFS_INDEX_CACHE_MODE != FFFS_INDEX_CACHE_HASH_HEADS
    return 0;
#else
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    const char *stale_name = "collision-38";
    const char *live_name = "collision-42";
    const char *stale_value = "stale";
    const char *live_value = "live";
    uint16_t stale_slot = fffs_hash16(stale_name);
    uint8_t out[16] = {0};
    size_t out_size = 0;
    uint16_t stale_head;
    bool found;

    uint16_t live_slot = fffs_hash16(live_name);
    ASSERT_TRUE((stale_slot & (TEST_INDEX_HASH_TABLE_SIZE - 1)) ==
            (live_slot & (TEST_INDEX_HASH_TABLE_SIZE - 1)));

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, stale_name, (const uint8_t *)stale_value,
                strlen(stale_value)));
    ASSERT_OK(fffs_index_head_for_slot(&fs, stale_slot, &stale_head, &found));
    ASSERT_TRUE(found);
    ASSERT_TRUE(stale_head >= fs.index_sectors);
    ASSERT_OK(write_chunks(&fs, live_name, (const uint8_t *)live_value,
                strlen(live_value)));
    ASSERT_OK(fffs_tombstone_metadata_for_slot(&fs, stale_head, stale_slot,
                FFFS_TOMBSTONE_NO_ACCOUNTING, NULL));
    fffs_unmount(&fs);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_OK(read_chunks(&remounted, live_name, out, sizeof(out),
                &out_size));
    ASSERT_TRUE(out_size == strlen(live_value));
    ASSERT_TRUE(memcmp(out, live_value, strlen(live_value)) == 0);

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
#endif
}

static int test_hash_remove_repairs_cluster_across_home_bucket(void) {
#if FFFS_INDEX_CACHE_MODE != FFFS_INDEX_CACHE_HASH_HEADS
    return 0;
#else
    static test_index_cache_t index_heads[TEST_INDEX_CACHE_WORDS];
    struct fffs fs = {
        .index_cache = index_heads,
        .index_hash_table_size = TEST_INDEX_HASH_TABLE_SIZE,
    };
    const uint16_t removed_slot = 27904; // home bucket 256
    const uint16_t middle_home_slot = 51457; // home bucket 257
    const uint16_t live_slot = 58624; // home bucket 256
    uint16_t head;
    bool found;

    memset(index_heads, 0, sizeof(index_heads));
    ASSERT_TRUE((removed_slot & (TEST_INDEX_HASH_TABLE_SIZE - 1)) == 256);
    ASSERT_TRUE((middle_home_slot & (TEST_INDEX_HASH_TABLE_SIZE - 1)) == 257);
    ASSERT_TRUE((live_slot & (TEST_INDEX_HASH_TABLE_SIZE - 1)) == 256);

    ASSERT_OK(fffs_index_insert(&fs, removed_slot, 41));
    ASSERT_OK(fffs_index_insert(&fs, middle_home_slot, 79));
    ASSERT_OK(fffs_index_insert(&fs, live_slot, 112));
    ASSERT_OK(fffs_index_remove(&fs, removed_slot));

    ASSERT_OK(fffs_index_head_for_slot(&fs, removed_slot, &head, &found));
    ASSERT_TRUE(!found);
    ASSERT_OK(fffs_index_head_for_slot(&fs, middle_home_slot, &head, &found));
    ASSERT_TRUE(found);
    ASSERT_TRUE(head == 79);
    ASSERT_OK(fffs_index_head_for_slot(&fs, live_slot, &head, &found));
    ASSERT_TRUE(found);
    ASSERT_TRUE(head == 112);
    return 0;
#endif
}

static int test_mount_uses_orphan_lookahead_for_serial_hint(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    const char *value = "committed";
    uint8_t orphan_footer_program[12] = {
        0xff, 0xff,
        0x2c, 0x01, 0x00, 0x00,
        0x01, 0x7e,
        'F', 'F', 'S', 'D',
    };
    uint16_t head = 0;
    uint16_t orphan_sector;
    bool found = false;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "config", (const uint8_t *)value,
                strlen(value)));
    ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16("config"), &head,
                &found));
    ASSERT_TRUE(found);
    orphan_sector = (uint16_t)(head + 1u);
    ASSERT_TRUE(orphan_sector < fs.sector_count);
    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash,
                    (size_t)orphan_sector * FFFS_DEFAULT_SECTOR_SIZE +
                    FFFS_DEFAULT_SECTOR_SIZE - sizeof(orphan_footer_program),
                    orphan_footer_program, sizeof(orphan_footer_program),
                    FFSV_CALLSITE)));
    fffs_unmount(&fs);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_TRUE(remounted.alloc_cursor >= remounted.index_sectors);
    ASSERT_TRUE(remounted.next_sector_serial == 301);

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_mount_discovers_sector_size(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint8_t data[] = {0x31, 0x32, 0x33};
    uint8_t out[8] = {0};
    size_t out_size = 0;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, &(struct fffs_format_options){
                .index_sectors = 2,
                .sector_size = FFFS_SECTOR_8K,
            }));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_TRUE(fs.sector_shift == 5);
    ASSERT_TRUE(fs.sector_size == 8192);
    ASSERT_OK(write_chunks(&fs, "wide-sector", data, sizeof(data)));
    fffs_unmount(&fs);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_TRUE(remounted.sector_shift == 5);
    ASSERT_TRUE(remounted.sector_size == 8192);
    ASSERT_OK(read_chunks(&remounted, "wide-sector", out, sizeof(out),
                &out_size));
    ASSERT_TRUE(out_size == sizeof(data));
    ASSERT_TRUE(memcmp(out, data, sizeof(data)) == 0);

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_mount_discovers_small_sector_size(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint8_t data[] = {0x41, 0x42, 0x43};
    uint8_t out[8] = {0};
    size_t out_size = 0;

    ASSERT_OK(new_backend_with_size(&flash, &backend, 256 * 128));
    ASSERT_OK(fffs_format(&backend, &(struct fffs_format_options){
                .index_sectors = 2,
                .sector_size = FFFS_SECTOR_256,
            }));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_TRUE(fs.sector_shift == 0);
    ASSERT_TRUE(fs.sector_size == 256);
    ASSERT_OK(write_chunks(&fs, "tiny-sector", data, sizeof(data)));
    fffs_unmount(&fs);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_TRUE(remounted.sector_shift == 0);
    ASSERT_TRUE(remounted.sector_size == 256);
    ASSERT_OK(read_chunks(&remounted, "tiny-sector", out, sizeof(out),
                &out_size));
    ASSERT_TRUE(out_size == sizeof(data));
    ASSERT_TRUE(memcmp(out, data, sizeof(data)) == 0);

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_format_tiny_sector_wins_over_old_large_remnant(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];

    ASSERT_OK(new_backend_with_size(&flash, &backend, 8192 * 8));
    ASSERT_OK(fffs_format(&backend, &(struct fffs_format_options){
                .index_sectors = 2,
                .sector_size = FFFS_SECTOR_8K,
            }));
    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash, 8192,
                    (const uint8_t[8]){'F', 'F', 'F', 'S', 1,
                        (uint8_t)(2 << 4 | 1), 5, 0x7f},
                    8, FFSV_CALLSITE)));
    ASSERT_OK(fffs_format(&backend, &(struct fffs_format_options){
                .index_sectors = 2,
                .sector_size = FFFS_SECTOR_256,
            }));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_TRUE(fs.sector_shift == 0);
    ASSERT_TRUE(fs.sector_size == 256);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_format_erases_expanded_index_area(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 16));
    ASSERT_OK(fffs_format(&backend, &(struct fffs_format_options){
                .index_sectors = 4,
                .sector_size = FFFS_SECTOR_DEFAULT,
            }));
    for (size_t sector = 1; sector < 4; sector++) {
        ASSERT_OK(flash_to_fs(ffsv_flash_program(flash,
                        sector * FFFS_DEFAULT_SECTOR_SIZE,
                        (const uint8_t[8]){'F', 'F', 'F', 'S', 1,
                            (uint8_t)(4 << 4 | sector),
                            FFFS_DEFAULT_SECTOR_SHIFT, 0x7f},
                        8, FFSV_CALLSITE)));
    }

    ASSERT_OK(fffs_format(&backend, &(struct fffs_format_options){
                .index_sectors = 8,
                .sector_size = FFFS_SECTOR_DEFAULT,
            }));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_TRUE(fs.index_sectors == 8);
    ASSERT_TRUE(fs.active_index_sector == 0);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_index_rotates_when_active_sector_fills(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    uint8_t out[4] = {0};
    size_t out_size = 0;
    const size_t writes = 1030;

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 1100));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    for (size_t i = 0; i < writes; i++) {
        uint8_t data = (uint8_t)i;
        ASSERT_OK(write_chunks(&fs, "rotating", &data, sizeof(data)));
    }
    ASSERT_TRUE(fs.active_index_sector == 1);
    ASSERT_TRUE(fs.active_index_serial == 1);
    fffs_unmount(&fs);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_TRUE(remounted.active_index_sector == 1);
    ASSERT_TRUE(remounted.active_index_serial == 1);
    ASSERT_OK(read_chunks(&remounted, "rotating", out, sizeof(out),
                &out_size));
    ASSERT_TRUE(out_size == 1);
    ASSERT_TRUE(out[0] == (uint8_t)(writes - 1));

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_index_rotation_commits_header_before_tombstone(void) {
    enum {
        sector_size = 256,
        sector_count = 256,
        source_records = (sector_size - TEST_INDEX_HEADER_SIZE) / 4,
    };
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    struct ffsv_flash_snapshot snapshot;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    char name[16];
    uint8_t data[1];

    ASSERT_OK(new_backend_with_sector_size(&flash, &backend, sector_size,
                sector_count));
    ASSERT_OK(fffs_format(&backend, &(struct fffs_format_options){
                .index_sectors = 2,
                .sector_size = FFFS_SECTOR_256,
            }));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    for (size_t i = 0; i + 1 < source_records; i++) {
        snprintf(name, sizeof(name), "rot%02zu", i);
        data[0] = (uint8_t)i;
        ASSERT_OK(write_chunks(&fs, name, data, sizeof(data)));
    }
    ASSERT_OK(fffs_delete_file(&fs, "rot00"));
    ASSERT_TRUE(fs.active_index_sector == 0);
    ASSERT_TRUE(fs.next_index_offset == sector_size);

    size_t log_before;
    const struct ffsv_op_record *log = ffsv_flash_log(flash, &log_before);
    (void)log;
    data[0] = 0xaa;
    ASSERT_OK(write_chunks(&fs, "trigger", data, sizeof(data)));

    size_t log_count;
    log = ffsv_flash_log(flash, &log_count);
    uint64_t copy_body_seq = 0;
    uint64_t header_body_seq = 0;
    uint64_t header_commit_seq = 0;
    uint64_t tombstone_seq = 0;
    for (size_t i = log_before; i < log_count; i++) {
        if (log[i].type != FFSV_OP_PROGRAM || log[i].result != FFSV_OK) {
            continue;
        }
        if (log[i].offset == sector_size && log[i].size == 8u) {
            header_body_seq = log[i].sequence;
        } else if (log[i].offset <= sector_size + 7u &&
                log[i].offset + log[i].size > sector_size + 7u) {
            header_commit_seq = log[i].sequence;
        } else if (log[i].offset > sector_size &&
                log[i].offset < 2u * sector_size) {
            if (copy_body_seq == 0) {
                copy_body_seq = log[i].sequence;
            }
        } else if (log[i].offset <= 7u &&
                log[i].offset + log[i].size > 7u) {
            tombstone_seq = log[i].sequence;
        }
    }
    ASSERT_TRUE(copy_body_seq != 0);
    ASSERT_TRUE(header_body_seq != 0);
    ASSERT_TRUE(header_commit_seq != 0);
    ASSERT_TRUE(tombstone_seq != 0);
    ASSERT_TRUE(header_body_seq > copy_body_seq);
    ASSERT_TRUE(header_commit_seq > header_body_seq);
    ASSERT_TRUE(tombstone_seq > header_commit_seq);

    ASSERT_OK(flash_to_fs(ffsv_flash_snapshot_create(flash, &snapshot)));
    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    flash = NULL;
    ASSERT_OK(flash_to_fs(ffsv_flash_reopen_from_snapshot(&flash,
                    &snapshot)));
    ffsv_flash_snapshot_destroy(&snapshot);
    ASSERT_OK(fffs_host_backend_from_verify_flash(&backend, flash));
    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_TRUE(remounted.active_index_sector == 1);
    size_t out_size = 0;
    ASSERT_OK(read_chunks(&remounted, "trigger", data, sizeof(data),
                &out_size));
    ASSERT_TRUE(out_size == 1);
    ASSERT_TRUE(data[0] == 0xaa);

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_mount_finishes_interrupted_index_compaction(void) {
    enum {
        sector_size = 256,
        sector_count = 256,
        source_records = (sector_size - TEST_INDEX_HEADER_SIZE) / 4,
    };
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct ffsv_flash_snapshot before_rotate;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    char name[16];
    uint8_t data[1];

    ASSERT_OK(new_backend_with_sector_size(&flash, &backend, sector_size,
                sector_count));
    ASSERT_OK(fffs_format(&backend, &(struct fffs_format_options){
                .index_sectors = 2,
                .sector_size = FFFS_SECTOR_256,
            }));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    for (size_t i = 0; i + 1 < source_records; i++) {
        snprintf(name, sizeof(name), "rec%02zu", i);
        data[0] = (uint8_t)i;
        ASSERT_OK(write_chunks(&fs, name, data, sizeof(data)));
    }
    ASSERT_OK(fffs_delete_file(&fs, "rec00"));
    ASSERT_TRUE(fs.active_index_sector == 0);
    ASSERT_TRUE(fs.next_index_offset == sector_size);

    ASSERT_OK(flash_to_fs(ffsv_flash_snapshot_create(flash,
                    &before_rotate)));
    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    flash = NULL;

    bool found_window = false;
    for (uint64_t delta = 0; delta < 512 && !found_window; delta++) {
        struct fffs attempt;
        struct ffsv_flash *attempt_flash = NULL;
        struct fffs_backend attempt_backend;
        static test_index_cache_t attempt_heads[TEST_INDEX_CACHE_WORDS];

        ASSERT_OK(flash_to_fs(ffsv_flash_reopen_from_snapshot(
                        &attempt_flash, &before_rotate)));
        ASSERT_OK(fffs_host_backend_from_verify_flash(&attempt_backend,
                    attempt_flash));
        ASSERT_OK(mount_fs(&attempt, &attempt_backend, attempt_heads));
        ffsv_flash_set_failure(attempt_flash,
                &(struct ffsv_failure_injection){
                    .enabled = true,
                    .sequence = ffsv_flash_next_sequence(attempt_flash) +
                        delta,
                    .op_mask = UINT32_C(1) << FFSV_OP_PROGRAM,
                    .phase = FFSV_FAIL_BEFORE,
                    .status = FFSV_ERR_INJECTED,
                });
        int err = fffs_rotate_index(&attempt);
        ffsv_flash_clear_failure(attempt_flash);

        bool new_valid = ffsv_flash_image_byte(attempt_flash,
                sector_size + 7u) == 0x7f;
        bool old_not_tombstoned = ffsv_flash_image_byte(attempt_flash, 7u) ==
            0x7f;
        if (err != FFFS_OK && new_valid && old_not_tombstoned) {
            struct fffs recovered;
            static test_index_cache_t recovered_heads[TEST_INDEX_CACHE_WORDS];
            found_window = true;

            fffs_unmount(&attempt);
            ASSERT_OK(mount_fs(&recovered, &attempt_backend,
                        recovered_heads));
            ASSERT_TRUE(recovered.oldest_index_sector == 1);
            ASSERT_TRUE(recovered.index_sequence_count == 1);
            ASSERT_TRUE(recovered.active_index_sector == 1);
            ASSERT_TRUE(ffsv_flash_image_byte(attempt_flash, 7u) == 0x3f);

            data[0] = 0xa5;
            ASSERT_OK(write_chunks(&recovered, "after", data, sizeof(data)));
            size_t out_size = 0;
            ASSERT_OK(read_chunks(&recovered, "rec01", data, sizeof(data),
                        &out_size));
            ASSERT_TRUE(out_size == 1);

            fffs_unmount(&recovered);
            ffsv_flash_destroy(attempt_flash);
            break;
        }

        fffs_unmount(&attempt);
        ffsv_flash_destroy(attempt_flash);
    }

    ffsv_flash_snapshot_destroy(&before_rotate);
    ASSERT_TRUE(found_window);
    return 0;
}

static int verify_index_crash_recovery_namespace(struct fffs *fs,
        size_t source_records) {
    char name[16];
    uint8_t out[4] = {0};
    size_t out_size = 0;
    size_t count = 0;
    bool trigger_exists = false;
    struct fffs_stat st;
    const uint8_t after[] = {0x5c};

    ASSERT_EQ_INT(FFFS_ERR_NOT_FOUND, fffs_stat(fs, "rec00", &st));
    for (size_t i = 1; i + 1 < source_records; i++) {
        snprintf(name, sizeof(name), "rec%02zu", i);
        memset(out, 0, sizeof(out));
        ASSERT_OK(read_chunks(fs, name, out, sizeof(out), &out_size));
        ASSERT_TRUE(out_size == 1);
        ASSERT_TRUE(out[0] == (uint8_t)i);
    }

    ASSERT_OK(fffs_exists(fs, "trigger", &trigger_exists));
    if (trigger_exists) {
        ASSERT_OK(read_chunks(fs, "trigger", out, sizeof(out), &out_size));
        ASSERT_TRUE(out_size == 1);
        ASSERT_TRUE(out[0] == 0xaa);
    }

    ASSERT_OK(count_dir(fs, NULL, &count));
    ASSERT_TRUE(count == source_records - 2u + (trigger_exists ? 1u : 0u));

    ASSERT_OK(write_chunks(fs, "after", after, sizeof(after)));
    ASSERT_OK(read_chunks(fs, "after", out, sizeof(out), &out_size));
    ASSERT_TRUE(out_size == sizeof(after));
    ASSERT_TRUE(out[0] == after[0]);
    return 0;
}

static int test_index_compaction_poweroff_after_each_flash_op(void) {
    enum {
        sector_size = 256,
        sector_count = 256,
        source_records = (sector_size - TEST_INDEX_HEADER_SIZE) / 4,
        max_mutations = source_records * 4,
    };
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct ffsv_flash_snapshot before_trigger;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    uint64_t mutation_sequences[max_mutations];
    size_t mutation_count = 0;
    char name[16];
    uint8_t data[1];

    ASSERT_OK(new_backend_with_sector_size(&flash, &backend, sector_size,
                sector_count));
    ASSERT_OK(fffs_format(&backend, &(struct fffs_format_options){
                .index_sectors = 2,
                .sector_size = FFFS_SECTOR_256,
            }));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    for (size_t i = 0; i + 1 < source_records; i++) {
        snprintf(name, sizeof(name), "rec%02zu", i);
        data[0] = (uint8_t)i;
        ASSERT_OK(write_chunks(&fs, name, data, sizeof(data)));
    }
    ASSERT_OK(fffs_delete_file(&fs, "rec00"));
    ASSERT_TRUE(fs.active_index_sector == 0);
    ASSERT_TRUE(fs.next_index_offset == sector_size);

    ASSERT_OK(flash_to_fs(ffsv_flash_snapshot_create(flash,
                    &before_trigger)));
    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    flash = NULL;

    ASSERT_OK(flash_to_fs(ffsv_flash_reopen_from_snapshot(&flash,
                    &before_trigger)));
    ASSERT_OK(fffs_host_backend_from_verify_flash(&backend, flash));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    size_t log_before = 0;
    (void)ffsv_flash_log(flash, &log_before);
    data[0] = 0xaa;
    ASSERT_OK(write_chunks(&fs, "trigger", data, sizeof(data)));
    size_t log_count = 0;
    const struct ffsv_op_record *log = ffsv_flash_log(flash, &log_count);
    for (size_t i = log_before; i < log_count; i++) {
        if (log[i].type != FFSV_OP_PROGRAM &&
                log[i].type != FFSV_OP_ERASE) {
            continue;
        }
        ASSERT_TRUE(mutation_count < max_mutations);
        mutation_sequences[mutation_count++] = log[i].sequence;
    }
    ASSERT_TRUE(mutation_count > 0);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    flash = NULL;

    for (size_t i = 0; i < mutation_count; i++) {
        for (size_t phase_i = 0; phase_i < 2; phase_i++) {
            enum ffsv_failure_phase phase = phase_i == 0 ?
                FFSV_FAIL_BEFORE : FFSV_FAIL_AFTER;
            struct ffsv_flash *attempt_flash = NULL;
            struct fffs_backend attempt_backend;
            struct fffs attempt;
            struct fffs recovered;
            static test_index_cache_t attempt_heads[TEST_INDEX_CACHE_WORDS];
            static test_index_cache_t recovered_heads[TEST_INDEX_CACHE_WORDS];

            ASSERT_OK(flash_to_fs(ffsv_flash_reopen_from_snapshot(
                            &attempt_flash, &before_trigger)));
            ASSERT_OK(fffs_host_backend_from_verify_flash(&attempt_backend,
                        attempt_flash));
            ASSERT_OK(mount_fs(&attempt, &attempt_backend, attempt_heads));

            ffsv_flash_set_failure(attempt_flash,
                    &(struct ffsv_failure_injection){
                        .enabled = true,
                        .sequence = mutation_sequences[i],
                        .op_mask = (UINT32_C(1) << FFSV_OP_PROGRAM) |
                            (UINT32_C(1) << FFSV_OP_ERASE),
                        .phase = phase,
                        .status = FFSV_ERR_INJECTED,
                    });
            data[0] = 0xaa;
            int write_err = write_chunks(&attempt, "trigger", data,
                    sizeof(data));
            ASSERT_EQ_INT(FFFS_ERR_IO, write_err);
            ffsv_flash_clear_failure(attempt_flash);

            fffs_unmount(&attempt);
            ASSERT_OK(mount_fs(&recovered, &attempt_backend,
                        recovered_heads));
            ASSERT_OK(verify_index_crash_recovery_namespace(&recovered,
                        source_records));

            fffs_unmount(&recovered);
            ffsv_flash_destroy(attempt_flash);
        }
    }

    ffsv_flash_snapshot_destroy(&before_trigger);
    return 0;
}

static int test_index_rotation_preserves_sequence_with_spare(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    uint8_t out[4] = {0};
    size_t out_size = 0;
    const size_t writes = 2045;

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 1200));
    ASSERT_OK(fffs_format(&backend, &(struct fffs_format_options){
                .index_sectors = 3,
            }));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    for (size_t i = 0; i < writes; i++) {
        uint8_t data = (uint8_t)i;
        ASSERT_OK(write_chunks(&fs, "rotating", &data, sizeof(data)));
    }
    ASSERT_TRUE(fs.oldest_index_sector == 1);
    ASSERT_TRUE(fs.index_sequence_count == 2);
    ASSERT_TRUE(fs.active_index_sector == 2);
    ASSERT_TRUE(fs.active_index_serial == 2);
    fffs_unmount(&fs);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_TRUE(remounted.oldest_index_sector == 1);
    ASSERT_TRUE(remounted.index_sequence_count == 2);
    ASSERT_TRUE(remounted.active_index_sector == 2);
    ASSERT_TRUE(remounted.active_index_serial == 2);
    ASSERT_OK(read_chunks(&remounted, "rotating", out, sizeof(out),
                &out_size));
    ASSERT_TRUE(out_size == 1);
    ASSERT_TRUE(out[0] == (uint8_t)(writes - 1));

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_index_independent_compaction_spills_and_commits_last(void) {
    enum {
        sector_size = 256,
        sector_count = 512,
        index_sectors = 3,
        source_records = (sector_size - TEST_INDEX_HEADER_SIZE) / 4,
        active_records = source_records - 1,
    };
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    struct ffsv_flash_snapshot snapshot;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    char name[16];
    uint8_t data[1];

    ASSERT_OK(new_backend_with_sector_size(&flash, &backend, sector_size,
                sector_count));
    ASSERT_OK(fffs_format(&backend, &(struct fffs_format_options){
                .index_sectors = index_sectors,
                .sector_size = FFFS_SECTOR_256,
            }));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    for (size_t i = 0; i < source_records; i++) {
        snprintf(name, sizeof(name), "src%02zu", i);
        data[0] = (uint8_t)i;
        ASSERT_OK(write_chunks(&fs, name, data, sizeof(data)));
    }
    for (size_t i = 0; i < active_records; i++) {
        snprintf(name, sizeof(name), "act%02zu", i);
        data[0] = (uint8_t)i;
        ASSERT_OK(write_chunks(&fs, name, data, sizeof(data)));
    }

    ASSERT_TRUE(fs.oldest_index_sector == 0);
    ASSERT_TRUE(fs.index_sequence_count == 2);
    ASSERT_TRUE(fs.active_index_sector == 1);
    ASSERT_TRUE(fs.next_index_offset == sector_size + TEST_INDEX_HEADER_SIZE +
            active_records * 4u);

    size_t log_before;
    const struct ffsv_op_record *log = ffsv_flash_log(flash, &log_before);
    (void)log;
    ASSERT_OK(fffs_index_compact_oldest(&fs));

    ASSERT_TRUE(fs.oldest_index_sector == 1);
    ASSERT_TRUE(fs.index_sequence_count == 2);
    ASSERT_TRUE(fs.active_index_sector == 2);
    ASSERT_TRUE(fs.next_index_offset == 2u * sector_size +
            TEST_INDEX_HEADER_SIZE + (source_records - 1u) * 4u);

    size_t log_count;
    log = ffsv_flash_log(flash, &log_count);
    uint64_t spill_body_seq = 0;
    uint64_t spill_header_body_seq = 0;
    uint64_t spill_header_commit_seq = 0;
    uint64_t source_tombstone_seq = 0;
    for (size_t i = log_before; i < log_count; i++) {
        if (log[i].type != FFSV_OP_PROGRAM || log[i].result != FFSV_OK) {
            continue;
        }
        if (log[i].offset == 2u * sector_size && log[i].size == 8u) {
            spill_header_body_seq = log[i].sequence;
        } else if (log[i].offset <= 2u * sector_size + 7u &&
                log[i].offset + log[i].size > 2u * sector_size + 7u) {
            spill_header_commit_seq = log[i].sequence;
        } else if (log[i].offset > 2u * sector_size &&
                log[i].offset < 3u * sector_size) {
            spill_body_seq = log[i].sequence;
        } else if (log[i].offset <= 7u &&
                log[i].offset + log[i].size > 7u) {
            source_tombstone_seq = log[i].sequence;
        }
    }
    ASSERT_TRUE(spill_body_seq != 0);
    ASSERT_TRUE(spill_header_body_seq != 0);
    ASSERT_TRUE(spill_header_commit_seq != 0);
    ASSERT_TRUE(source_tombstone_seq != 0);
    ASSERT_TRUE(spill_header_body_seq > spill_body_seq);
    ASSERT_TRUE(spill_header_commit_seq > spill_header_body_seq);
    ASSERT_TRUE(source_tombstone_seq > spill_header_commit_seq);

    ASSERT_OK(flash_to_fs(ffsv_flash_snapshot_create(flash, &snapshot)));
    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    flash = NULL;
    ASSERT_OK(flash_to_fs(ffsv_flash_reopen_from_snapshot(&flash,
                    &snapshot)));
    ffsv_flash_snapshot_destroy(&snapshot);
    ASSERT_OK(fffs_host_backend_from_verify_flash(&backend, flash));
    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_TRUE(remounted.oldest_index_sector == 1);
    ASSERT_TRUE(remounted.index_sequence_count == 2);
    ASSERT_TRUE(remounted.active_index_sector == 2);

    for (size_t i = 0; i < source_records; i++) {
        uint8_t out[4] = {0};
        size_t out_size = 0;
        snprintf(name, sizeof(name), "src%02zu", i);
        ASSERT_OK(read_chunks(&remounted, name, out, sizeof(out),
                    &out_size));
        ASSERT_TRUE(out_size == 1);
        ASSERT_TRUE(out[0] == (uint8_t)i);
    }
    for (size_t i = 0; i < active_records; i++) {
        uint8_t out[4] = {0};
        size_t out_size = 0;
        snprintf(name, sizeof(name), "act%02zu", i);
        ASSERT_OK(read_chunks(&remounted, name, out, sizeof(out),
                    &out_size));
        ASSERT_TRUE(out_size == 1);
        ASSERT_TRUE(out[0] == (uint8_t)i);
    }

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_index_compaction_mixed_history_metrics(void) {
    enum {
        file_count = 256,
        event_count = 1000,
    };
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    bool live[file_count] = {0};
    size_t live_count = 0;
    uint32_t rng = 0x51f0ca7eu;
    uint8_t value[4];
    char name[16];
    struct measured_ops before_ops;
    struct measured_ops after_ops;
    struct measured_ops ops;
    struct fffs_inspect_summary summary;

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 1400));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    for (size_t event = 0; event < event_count; event++) {
        rng = rng * 1664525u + 1013904223u;
        size_t id = (size_t)(rng % file_count);
        snprintf(name, sizeof(name), "mix%03zu", id);
        if (live[id] && (rng & 7u) == 0u) {
            ASSERT_OK(fffs_delete_file(&fs, name));
            live[id] = false;
            live_count--;
            continue;
        }
        value[0] = (uint8_t)rng;
        value[1] = (uint8_t)(rng >> 8);
        value[2] = (uint8_t)event;
        value[3] = (uint8_t)(event >> 8);
        ASSERT_OK(write_chunks(&fs, name, value, sizeof(value)));
        if (!live[id]) {
            live[id] = true;
            live_count++;
        }
    }
    ASSERT_TRUE(live_count > 0);
    ASSERT_TRUE(live_count < event_count);

    ASSERT_TRUE(fs.index_sequence_count == 1);
    ASSERT_TRUE(fs.next_index_offset == fs.active_index_sector *
            fs.sector_size + TEST_INDEX_HEADER_SIZE + event_count * 4u);
    capture_ops(flash, &before_ops);
    ASSERT_OK(fffs_rotate_index(&fs));
    capture_ops(flash, &after_ops);
    diff_ops(&before_ops, &after_ops, &ffsv_flash_config(flash)->timing,
            &ops);

    ASSERT_TRUE(fs.index_sequence_count == 1);
    ASSERT_OK(fffs_inspect_check(&backend, &summary));
    ASSERT_TRUE(summary.live_entries == live_count);
    ASSERT_TRUE(summary.index_records == live_count);
    ASSERT_TRUE(summary.index_deletes == 0);
    ASSERT_TRUE(summary.live_entries_corrupt == 0);
    ASSERT_TRUE(summary.md_corrupt == 0);
    ASSERT_TRUE(ops.calls[FFSV_OP_PROGRAM] <= live_count * 2u + 4u);

    char duration[32];
    char op_summary[192] = "";
    bool first = true;
    format_duration(duration, sizeof(duration), ops.ns);
    append_measured_op(op_summary, sizeof(op_summary), &first, "r", &ops,
            FFSV_OP_READ);
    append_measured_op(op_summary, sizeof(op_summary), &first, "p", &ops,
            FFSV_OP_PROGRAM);
    append_measured_op(op_summary, sizeof(op_summary), &first, "e", &ops,
            FFSV_OP_ERASE);
    append_measured_op(op_summary, sizeof(op_summary), &first, "bc", &ops,
            FFSV_OP_BLANK_CHECK);

    fprintf(stderr,
            "index compaction mixed history live=%u  %10s   %s\n",
            (unsigned)summary.live_entries, duration,
            first ? "no flash ops" : op_summary);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_index_header_discovery_without_sector_zero(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint8_t data[] = {9, 8, 7, 6};
    uint8_t header[8] = {'F', 'F', 'F', 'S', 1,
        (uint8_t)(2 << 4 | 1), FFFS_DEFAULT_SECTOR_SHIFT, 0x7f};

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(flash_to_fs(ffsv_flash_erase(flash, 0,
                    ffsv_flash_size(flash), FFSV_CALLSITE)));
    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash, 4096, header,
                    sizeof(header), FFSV_CALLSITE)));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "moved-index", data, sizeof(data)));

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_uncommitted_index_header_is_not_discovered(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    uint8_t header[8] = {'F', 'F', 'F', 'S', 1,
        (uint8_t)(2 << 4 | 1), FFFS_DEFAULT_SECTOR_SHIFT, 0xff};

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(flash_to_fs(ffsv_flash_erase(flash, 0,
                    ffsv_flash_size(flash), FFSV_CALLSITE)));
    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash, 4096, header,
                    sizeof(header), FFSV_CALLSITE)));
    ASSERT_EQ_INT(FFFS_ERR_CORRUPT,
            mount_fs(&fs, &backend, fs_index_heads));

    ffsv_flash_destroy(flash);
    return 0;
}

static void fill_large_pattern(uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        data[i] = (uint8_t)(i * 31u + i / 7u);
    }
}

static int test_large_file_uses_noncontiguous_extents(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static test_index_cache_t remount_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint8_t tiny[] = {0x42};
    const size_t large_size = 350u * 1024u;
    uint8_t *large = malloc(large_size);
    uint8_t *out = malloc(large_size);
    size_t out_size = 0;
    struct fffs_inspect_summary inspect;

    ASSERT_TRUE(large != NULL);
    ASSERT_TRUE(out != NULL);
    fill_large_pattern(large, large_size);

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 128));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    fs.alloc_cursor = fs.index_sectors + 2;
    ASSERT_OK(write_chunks(&fs, "blocker", tiny, sizeof(tiny)));
    ASSERT_TRUE(fs.alloc_cursor == fs.index_sectors + 2);
    fs.alloc_cursor = fs.index_sectors + 1;
    ASSERT_OK(write_chunks(&fs, "large.bin", large, large_size));
    ASSERT_OK(read_chunks(&fs, "large.bin", out, large_size, &out_size));
    ASSERT_TRUE(out_size == large_size);
    ASSERT_TRUE(memcmp(out, large, large_size) == 0);
    ASSERT_OK(fffs_inspect_check(&backend, &inspect));
    ASSERT_TRUE(inspect.live_entries_corrupt == 0);
    ASSERT_TRUE(inspect.md_corrupt == 0);
    ASSERT_TRUE(inspect.md_live > inspect.live_entries);
    fffs_unmount(&fs);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    memset(out, 0, large_size);
    ASSERT_OK(read_chunks(&remounted, "large.bin", out, large_size,
                &out_size));
    ASSERT_TRUE(out_size == large_size);
    ASSERT_TRUE(memcmp(out, large, large_size) == 0);

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    free(out);
    free(large);
    return 0;
}

static int test_large_file_uses_contiguous_spans(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const size_t large_size = 32u * 1024u;
    uint8_t *large = malloc(large_size);
    uint8_t out[16] = {0};
    size_t nread = 0;
    uint32_t pos = 0;
    struct fffs_md_record span_record;
    struct fffs_inspect_summary inspect;

    ASSERT_TRUE(large != NULL);
    fill_large_pattern(large, large_size);

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 32));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "large.bin", large, large_size));

    ASSERT_OK(fffs_open(&fs, &file, "large.bin", FFFS_O_RDONLY));
    ASSERT_OK(fffs_read_md_for_slot(&fs, file.head, file.slot,
                &span_record));
    ASSERT_TRUE(span_record.span_len > 1);
    ASSERT_TRUE(span_record.data_len > 0);

    uint32_t target = 8192u + 17u;
    ASSERT_TRUE(target < large_size);
    uint64_t reads_before = ffsv_flash_counts(flash)[FFSV_OP_READ].calls;
    ASSERT_OK(fffs_seek(&file, (int32_t)target, FFFS_SEEK_SET, &pos));
    ASSERT_TRUE(pos == target);
    ASSERT_TRUE(ffsv_flash_counts(flash)[FFSV_OP_READ].calls <=
            reads_before + 3u);
    ASSERT_OK(fffs_read(&file, out, sizeof(out), &nread));
    ASSERT_TRUE(nread == sizeof(out));
    ASSERT_TRUE(memcmp(out, large + target, nread) == 0);

    target += 4096u;
    ASSERT_TRUE(target < large_size);
    reads_before = ffsv_flash_counts(flash)[FFSV_OP_READ].calls;
    ASSERT_OK(fffs_seek(&file, (int32_t)target, FFFS_SEEK_SET, &pos));
    ASSERT_TRUE(pos == target);
    ASSERT_TRUE(ffsv_flash_counts(flash)[FFSV_OP_READ].calls <=
            reads_before + 3u);
    ASSERT_OK(fffs_close(&file));

    ASSERT_OK(fffs_inspect_check(&backend, &inspect));
    ASSERT_TRUE(inspect.live_entries_corrupt == 0);
    ASSERT_TRUE(inspect.md_corrupt == 0);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    free(large);
    return 0;
}

static int test_span_head_skips_contiguous_continuations(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const size_t large_size = 24u * 1024u;
    uint8_t *large = malloc(large_size);
    struct fffs_md_record root_record;
    struct fffs_md_record cont_record;

    ASSERT_TRUE(large != NULL);
    fill_large_pattern(large, large_size);

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 32));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "large.bin", large, large_size));

    ASSERT_OK(fffs_open(&fs, &file, "large.bin", FFFS_O_RDONLY));
    ASSERT_OK(fffs_read_md_for_slot(&fs, file.head, file.slot,
                &root_record));
    ASSERT_TRUE(root_record.span_len > 1);
    ASSERT_TRUE(root_record.next == 0);

    ASSERT_OK(fffs_read_md_for_slot(&fs, (uint16_t)(file.head + 1u),
            file.slot, &cont_record));
    ASSERT_TRUE(cont_record.span_len == 1);
    ASSERT_TRUE(cont_record.next == file.head + 2u);
    ASSERT_OK(fffs_close(&file));

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    free(large);
    return 0;
}

static int test_root_alloc_skips_sector_with_continuation_record(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file large_file;
    struct fffs_file tiny_file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    size_t large_size;
    uint8_t *large;
    uint8_t tiny = 0x51;
    struct fffs_md_record cont_record;
    uint16_t continuation_sector;

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 32));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    large_size = fs.sector_size + 512u;
    large = malloc(large_size);
    ASSERT_TRUE(large != NULL);
    fill_large_pattern(large, large_size);

    ASSERT_OK(fffs_open(&fs, &large_file, "large.bin",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    fs.alloc_cursor = (uint16_t)(large_file.head + 2u);
    ASSERT_OK(fffs_write(&large_file, large, large_size));
    ASSERT_TRUE(large_file.current != large_file.head);
    continuation_sector = large_file.current;
    ASSERT_OK(fffs_close(&large_file));

    ASSERT_OK(fffs_open(&fs, &large_file, "large.bin", FFFS_O_RDONLY));
    ASSERT_OK(fffs_read_md_for_slot(&fs, continuation_sector, large_file.slot,
                &cont_record));
    ASSERT_TRUE(cont_record.type == TEST_MD_TYPE_FILE_CONT_V1);
    ASSERT_OK(fffs_close(&large_file));

    fs.alloc_cursor = continuation_sector;
    ASSERT_OK(fffs_open(&fs, &tiny_file, "tiny",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_TRUE(tiny_file.head != continuation_sector);
    ASSERT_OK(fffs_write(&tiny_file, &tiny, sizeof(tiny)));
    ASSERT_OK(fffs_close(&tiny_file));

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    free(large);
    return 0;
}

static int test_continuation_alloc_requires_empty_sector(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    uint8_t small = 0x32;
    const size_t large_size = 4096u * 2u;
    uint8_t *large = malloc(large_size);
    uint16_t root_only_sector;
    uint16_t head = 0;
    bool found = false;

    ASSERT_TRUE(large != NULL);
    fill_large_pattern(large, large_size);

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 32));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    fs.alloc_cursor = (uint16_t)(fs.index_sectors + 1u);
    ASSERT_OK(write_chunks(&fs, "root-only", &small, sizeof(small)));
    ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16("root-only"),
                &head, &found));
    ASSERT_TRUE(found);
    root_only_sector = head;

    fs.alloc_cursor = fs.index_sectors;
    ASSERT_OK(fffs_open(&fs, &file, "large.bin",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_TRUE(file.head != root_only_sector);
    ASSERT_OK(fffs_write(&file, large, large_size));
    ASSERT_TRUE(file.current != file.head);
    uint16_t root_sector = file.head;
    uint16_t continuation_sector = file.current;
    ASSERT_TRUE(file.current != root_only_sector);
    ASSERT_OK(fffs_close(&file));
    ASSERT_TRUE(ffsv_flash_image_byte(flash, (size_t)root_sector *
                fs.sector_size + fs.sector_size -
                TEST_SECTOR_FOOTER_SIZE + 5u) == TEST_SECTOR_FLAGS_FULL);
    ASSERT_TRUE(ffsv_flash_image_byte(flash, (size_t)continuation_sector *
                fs.sector_size + fs.sector_size -
                TEST_SECTOR_FOOTER_SIZE + 5u) == TEST_SECTOR_FLAGS_FULL);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    free(large);
    return 0;
}

static int test_gc_compacts_root_only_sector_under_pressure(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file large_file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint8_t value[] = {0x10, 0x11, 0x12};
    size_t large_size;
    uint8_t *large;
    uint8_t out[8] = {0};
    size_t out_size = 0;
    uint16_t source_sector;
    uint16_t continuation_sector;
    uint16_t erased_sector = 0;
    uint16_t head = 0;
    uint16_t moved_head = 0;
    bool found = false;
    struct fffs_inspect_summary inspect;
    struct measured_ops before_ops;
    struct measured_ops after_ops;
    struct measured_ops ops;

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 32));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    fs.alloc_cursor = fs.index_sectors;
    ASSERT_OK(write_chunks(&fs, "f000", value, sizeof(value)));
    ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16("f000"), &head,
                &found));
    ASSERT_TRUE(found);
    source_sector = head;
    for (size_t i = 1; i < 8; i++) {
        char name[8];
        snprintf(name, sizeof(name), "f%03zu", i);
        fs.alloc_cursor = fs.index_sectors;
        ASSERT_OK(write_chunks(&fs, name, value, sizeof(value)));
        ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16(name), &head,
                    &found));
        ASSERT_TRUE(found);
        ASSERT_TRUE(head == source_sector);
    }
    for (size_t i = 0; i < 6; i++) {
        char name[8];
        snprintf(name, sizeof(name), "f%03zu", i);
        ASSERT_OK(fffs_delete_file(&fs, name));
    }

    large_size = fs.sector_size + 512u;
    large = malloc(large_size);
    ASSERT_TRUE(large != NULL);
    fill_large_pattern(large, large_size);

    fs.alloc_cursor = (uint16_t)(source_sector + 1u);
    ASSERT_OK(fffs_open(&fs, &large_file, "large.bin",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_OK(fffs_write(&large_file, large, large_size));
    ASSERT_TRUE(large_file.current != large_file.head);
    continuation_sector = large_file.current;
    ASSERT_OK(fffs_close(&large_file));

    for (size_t sector = (size_t)continuation_sector + 1u;
            sector < fs.sector_count; sector++) {
        char name[16];
        snprintf(name, sizeof(name), "fill%02zu", sector);
        fs.alloc_cursor = sector;
        ASSERT_OK(write_chunks(&fs, name, large,
                    test_max_file_data_size(&fs)));
        ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16(name), &head,
                    &found));
        ASSERT_TRUE(found);
        ASSERT_TRUE(head == sector);
        ASSERT_TRUE(ffsv_flash_image_byte(flash, sector * fs.sector_size +
                    fs.sector_size - TEST_SECTOR_FOOTER_SIZE + 5u) ==
                TEST_SECTOR_FLAGS_FULL);
    }

    fs.alloc_cursor = continuation_sector;
    fs.gc_cursor = source_sector;
    capture_ops(flash, &before_ops);
    ASSERT_OK(fffs_gc_until_erased(&fs, &erased_sector));
    capture_ops(flash, &after_ops);
    diff_ops(&before_ops, &after_ops, &ffsv_flash_config(flash)->timing,
            &ops);
    ASSERT_TRUE(erased_sector == source_sector);

    ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16("f006"), &head,
                &found));
    ASSERT_TRUE(found);
    ASSERT_TRUE(head != source_sector);
    moved_head = head;
    ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16("f007"), &head,
                &found));
    ASSERT_TRUE(found);
    ASSERT_TRUE(head != source_sector);

    ASSERT_OK(read_chunks(&fs, "f006", out, sizeof(out), &out_size));
    ASSERT_TRUE(out_size == sizeof(value));
    ASSERT_TRUE(memcmp(out, value, sizeof(value)) == 0);
    memset(out, 0, sizeof(out));
    ASSERT_OK(read_chunks(&fs, "f007", out, sizeof(out), &out_size));
    ASSERT_TRUE(out_size == sizeof(value));
    ASSERT_TRUE(memcmp(out, value, sizeof(value)) == 0);
    ASSERT_OK(fffs_inspect_check(&backend, &inspect));
    ASSERT_TRUE(inspect.live_entries_corrupt == 0);
    ASSERT_TRUE(inspect.md_corrupt == 0);

    char duration[32];
    char op_summary[192] = "";
    bool first = true;
    format_duration(duration, sizeof(duration), ops.ns);
    append_measured_op(op_summary, sizeof(op_summary), &first, "r", &ops,
            FFSV_OP_READ);
    append_measured_op(op_summary, sizeof(op_summary), &first, "p", &ops,
            FFSV_OP_PROGRAM);
    append_measured_op(op_summary, sizeof(op_summary), &first, "e", &ops,
            FFSV_OP_ERASE);
    append_measured_op(op_summary, sizeof(op_summary), &first, "bc", &ops,
            FFSV_OP_BLANK_CHECK);

    fprintf(stderr,
            "root-only compaction roots=2 source=%u dest=%u  %10s   %s\n",
            (unsigned)source_sector, (unsigned)moved_head,
            duration, first ? "no flash ops" : op_summary);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    free(large);
    return 0;
}

static int test_compaction_preserves_open_reader_data(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file large_file;
    struct fffs_file reader;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint8_t value[] = {0x10, 0x11, 0x12};
    uint8_t survivor[512];
    uint8_t out[512];
    size_t large_size;
    uint8_t *large;
    size_t nread = 0;
    size_t total = 0;
    uint16_t source_sector;
    uint16_t continuation_sector;
    uint16_t erased_sector = 0;
    uint16_t head = 0;
    bool found = false;
    int rc;

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 32));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    fill_large_pattern(survivor, sizeof(survivor));

    fs.alloc_cursor = fs.index_sectors;
    ASSERT_OK(write_chunks(&fs, "f000", value, sizeof(value)));
    ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16("f000"), &head,
                &found));
    ASSERT_TRUE(found);
    source_sector = head;
    for (size_t i = 1; i < 6; i++) {
        char name[8];
        snprintf(name, sizeof(name), "f%03zu", i);
        fs.alloc_cursor = fs.index_sectors;
        ASSERT_OK(write_chunks(&fs, name, value, sizeof(value)));
        ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16(name), &head,
                    &found));
        ASSERT_TRUE(found);
        ASSERT_TRUE(head == source_sector);
    }
    fs.alloc_cursor = fs.index_sectors;
    ASSERT_OK(write_chunks(&fs, "f006", survivor, sizeof(survivor)));
    ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16("f006"), &head,
                &found));
    ASSERT_TRUE(found);
    ASSERT_TRUE(head == source_sector);
    for (size_t i = 0; i < 6; i++) {
        char name[8];
        snprintf(name, sizeof(name), "f%03zu", i);
        ASSERT_OK(fffs_delete_file(&fs, name));
    }

    large_size = fs.sector_size + 512u;
    large = malloc(large_size);
    ASSERT_TRUE(large != NULL);
    fill_large_pattern(large, large_size);

    fs.alloc_cursor = (uint16_t)(source_sector + 1u);
    ASSERT_OK(fffs_open(&fs, &large_file, "large.bin",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_OK(fffs_write(&large_file, large, large_size));
    ASSERT_TRUE(large_file.current != large_file.head);
    continuation_sector = large_file.current;
    ASSERT_OK(fffs_close(&large_file));

    for (size_t sector = (size_t)continuation_sector + 1u;
            sector < fs.sector_count; sector++) {
        char name[16];
        snprintf(name, sizeof(name), "fill%02zu", sector);
        fs.alloc_cursor = sector;
        ASSERT_OK(write_chunks(&fs, name, large,
                    test_max_file_data_size(&fs)));
    }

    ASSERT_OK(fffs_open(&fs, &reader, "f006", FFFS_O_RDONLY));
    ASSERT_OK(fffs_read(&reader, out, 2, &nread));
    ASSERT_TRUE(nread == 2);
    ASSERT_TRUE(memcmp(out, survivor, 2) == 0);
    total = 2;

    fs.alloc_cursor = continuation_sector;
    fs.gc_cursor = source_sector;
    rc = fffs_gc_until_erased(&fs, &erased_sector);
    found = false;
    head = 0;
    ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16("f006"), &head,
                &found));
    fprintf(stderr, "open reader gc rc=%s erased=%u source=%u f006 head=%u\n",
            fffs_status_name(rc), (unsigned)erased_sector,
            (unsigned)source_sector, (unsigned)head);

    while (total < sizeof(survivor)) {
        nread = 0;
        ASSERT_OK(fffs_read(&reader, out + total, 64, &nread));
        ASSERT_TRUE(nread > 0);
        total += nread;
    }
    ASSERT_TRUE(total == sizeof(survivor));
    ASSERT_TRUE(memcmp(out, survivor, sizeof(survivor)) == 0);
    ASSERT_OK(fffs_close(&reader));

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    free(large);
    return 0;
}

static int test_open_writer_close_preserves_reused_slot_file(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file writer;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    uint8_t a0[16];
    uint8_t a1[16];
    uint8_t b[16];
    uint8_t out[32];
    size_t out_size = 0;
    char peer[16];
    bool peer_found = false;
    uint16_t base;

    memset(a0, 0xa0, sizeof(a0));
    memset(a1, 0xa1, sizeof(a1));
    memset(b, 0xb5, sizeof(b));

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 16));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    base = fffs_hash16("victim");
    ASSERT_TRUE(base != 0 && base != 0xffff);

    ASSERT_OK(write_chunks(&fs, "victim", a0, sizeof(a0)));
    ASSERT_OK(fffs_open(&fs, &writer, "victim",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_OK(fffs_write(&writer, a1, sizeof(a1)));
    ASSERT_OK(fffs_delete_file(&fs, "victim"));

    for (unsigned i = 0; i < 1000000u; i++) {
        snprintf(peer, sizeof(peer), "c%06u", i);
        if (fffs_hash16(peer) == base) {
            peer_found = true;
            break;
        }
    }
    ASSERT_TRUE(peer_found);

    ASSERT_OK(write_chunks(&fs, peer, b, sizeof(b)));
    ASSERT_OK(fffs_close(&writer));

    ASSERT_OK(read_chunks(&fs, peer, out, sizeof(out), &out_size));
    ASSERT_TRUE(out_size == sizeof(b));
    ASSERT_TRUE(memcmp(out, b, sizeof(b)) == 0);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_compaction_window_consumes_reserved_sector(void) {
#if FFFS_COMPACTION_RESERVE_SECTORS <= 0
    return 0;
#else
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    uint16_t sector = 0;
    uint16_t data_off = 0;
    uint16_t record_off = 0;
    bool needs_footer = false;
    uint16_t reserved_sector;

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 32));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    reserved_sector = fs.index_sectors;
    fs.compaction_reserve_count = 1;
    fs.compaction_reserve_sectors[0] = reserved_sector;

    ASSERT_OK(fffs_alloc_find_compaction_root_window(&fs,
                (uint16_t)(reserved_sector + 1u), fffs_hash16("moved"),
                16, &sector, &data_off, &record_off, &needs_footer, true));
    ASSERT_TRUE(sector == reserved_sector);
    ASSERT_TRUE(fs.compaction_reserve_count == 0);
    ASSERT_TRUE(data_off == 0);
    ASSERT_TRUE(needs_footer);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
#endif
}

static int test_replacement_refreshes_old_head_after_alloc_gc(void) {
#if !FFFS_GC_ON_ALLOC_FAILURE || FFFS_COMPACTION_RESERVE_SECTORS > 0
    return 0;
#else
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    static uint8_t large[FFFS_DEFAULT_SECTOR_SIZE];
    const uint8_t tiny = 0x21;
    const uint8_t other_live[96] = {0x43};
    const uint8_t replacement[] = {0x91, 0x92, 0x93};
    uint8_t out[8] = {0};
    size_t out_size = 0;
    uint16_t source_sector = 0;
    uint16_t other_sector = 0;
    uint16_t head = 0;
    bool found = false;
    struct fffs_inspect_summary inspect;
    char name[32];

    memset(large, 0x68, sizeof(large));
    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 32));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    fs.alloc_cursor = fs.index_sectors;
    ASSERT_OK(write_chunks(&fs, "target", &tiny, sizeof(tiny)));
    ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16("target"), &head,
                &found));
    ASSERT_TRUE(found);
    source_sector = head;
    for (size_t i = 0; i < FFFS_ALLOC_MAX_MD_RECORDS_PER_SECTOR - 1u; i++) {
        snprintf(name, sizeof(name), "src%02zu", i);
        fs.alloc_cursor = fs.index_sectors;
        ASSERT_OK(write_chunks(&fs, name, &tiny, sizeof(tiny)));
        ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16(name), &head,
                    &found));
        ASSERT_TRUE(found);
        ASSERT_TRUE(head == source_sector);
    }

    fs.alloc_cursor = (uint16_t)(source_sector + 1u);
    ASSERT_OK(write_chunks(&fs, "other-live", other_live,
                sizeof(other_live)));
    ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16("other-live"),
                &head, &found));
    ASSERT_TRUE(found);
    ASSERT_TRUE(head != source_sector);
    other_sector = head;
    for (size_t i = 0; i < FFFS_ALLOC_MAX_MD_RECORDS_PER_SECTOR - 1u; i++) {
        snprintf(name, sizeof(name), "oth%02zu", i);
        fs.alloc_cursor = other_sector;
        ASSERT_OK(write_chunks(&fs, name, &tiny, sizeof(tiny)));
        ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16(name), &head,
                    &found));
        ASSERT_TRUE(found);
        ASSERT_TRUE(head == other_sector);
    }

    for (size_t i = 0; i < FFFS_ALLOC_MAX_MD_RECORDS_PER_SECTOR - 1u; i++) {
        snprintf(name, sizeof(name), "src%02zu", i);
        ASSERT_OK(fffs_delete_file(&fs, name));
        snprintf(name, sizeof(name), "oth%02zu", i);
        ASSERT_OK(fffs_delete_file(&fs, name));
    }

    size_t large_size = sizeof(large);
    ASSERT_TRUE(large_size <= sizeof(large));
    for (size_t i = 0; i < 14; i++) {
        snprintf(name, sizeof(name), "fill%02zu", i);
        ASSERT_OK(write_chunks(&fs, name, large, large_size));
    }

    fs.alloc_cursor = fs.index_sectors;
    fs.gc_cursor = source_sector;
    ASSERT_OK(fffs_open(&fs, &file, "target",
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    ASSERT_TRUE(file.old_head == file.head);
    ASSERT_OK(fffs_write(&file, replacement, sizeof(replacement)));
    ASSERT_OK(fffs_close(&file));

    ASSERT_OK(fffs_inspect_check(&backend, &inspect));
    ASSERT_TRUE(inspect.live_entries_corrupt == 0);
    ASSERT_TRUE(inspect.md_corrupt == 0);
    ASSERT_OK(read_chunks(&fs, "target", out, sizeof(out), &out_size));
    ASSERT_TRUE(out_size == sizeof(replacement));
    ASSERT_TRUE(memcmp(out, replacement, sizeof(replacement)) == 0);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
#endif
}

static int test_read_seek_single_extent(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const uint8_t data[] = "abcdefghijklmnopqrstuvwxyz";
    uint8_t out[8] = {0};
    size_t nread = 0;
    uint32_t pos = 0;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "letters", data, sizeof(data) - 1));

    ASSERT_OK(fffs_open(&fs, &file, "letters", FFFS_O_RDONLY));
    ASSERT_OK(fffs_read(&file, out, 5, &nread));
    ASSERT_TRUE(nread == 5);
    ASSERT_TRUE(memcmp(out, "abcde", 5) == 0);

    uint64_t reads_before = ffsv_flash_counts(flash)[FFSV_OP_READ].calls;
    ASSERT_OK(fffs_seek(&file, 10, FFFS_SEEK_SET, &pos));
    ASSERT_TRUE(ffsv_flash_counts(flash)[FFSV_OP_READ].calls ==
            reads_before);
    ASSERT_TRUE(pos == 10);
    ASSERT_OK(fffs_read(&file, out, 4, &nread));
    ASSERT_TRUE(nread == 4);
    ASSERT_TRUE(memcmp(out, "klmn", 4) == 0);

    reads_before = ffsv_flash_counts(flash)[FFSV_OP_READ].calls;
    ASSERT_OK(fffs_seek(&file, -2, FFFS_SEEK_CUR, &pos));
    ASSERT_TRUE(ffsv_flash_counts(flash)[FFSV_OP_READ].calls ==
            reads_before);
    ASSERT_TRUE(pos == 12);
    ASSERT_OK(fffs_read(&file, out, 3, &nread));
    ASSERT_TRUE(nread == 3);
    ASSERT_TRUE(memcmp(out, "mno", 3) == 0);

    ASSERT_OK(fffs_seek(&file, -3, FFFS_SEEK_END, &pos));
    ASSERT_TRUE(pos == 23);
    ASSERT_OK(fffs_read(&file, out, sizeof(out), &nread));
    ASSERT_TRUE(nread == 3);
    ASSERT_TRUE(memcmp(out, "xyz", 3) == 0);
    ASSERT_EQ_INT(FFFS_ERR_RANGE,
            fffs_seek(&file, -27, FFFS_SEEK_END, NULL));
    ASSERT_EQ_INT(FFFS_ERR_RANGE,
            fffs_seek(&file, 1, FFFS_SEEK_END, NULL));
    ASSERT_OK(fffs_close(&file));

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_read_seek_across_extents(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs_file file;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const size_t large_size = 12u * 1024u;
    uint8_t *large = malloc(large_size);
    uint8_t out[64] = {0};
    size_t nread = 0;
    uint32_t pos = 0;

    ASSERT_TRUE(large != NULL);
    fill_large_pattern(large, large_size);

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 16));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "large.bin", large, large_size));

    ASSERT_OK(fffs_open(&fs, &file, "large.bin", FFFS_O_RDONLY));
    ASSERT_TRUE(file.current_data_len > 16);
    ASSERT_TRUE(file.current_data_len < large_size);
    uint32_t target = file.current_data_len - 7u;
    ASSERT_OK(fffs_seek(&file, (int32_t)target, FFFS_SEEK_SET, &pos));
    ASSERT_TRUE(pos == target);
    ASSERT_OK(fffs_read(&file, out, 32, &nread));
    ASSERT_TRUE(nread == 32);
    ASSERT_TRUE(memcmp(out, large + target, nread) == 0);

    ASSERT_OK(fffs_seek(&file, -10, FFFS_SEEK_CUR, &pos));
    ASSERT_TRUE(pos == target + 22u);
    ASSERT_OK(fffs_read(&file, out, 17, &nread));
    ASSERT_TRUE(nread == 17);
    ASSERT_TRUE(memcmp(out, large + pos, nread) == 0);

    uint32_t same_span_target = file.current_file_offset + 4096u + 5u;
    ASSERT_TRUE(same_span_target < large_size);
    uint64_t reads_before = ffsv_flash_counts(flash)[FFSV_OP_READ].calls;
    ASSERT_OK(fffs_seek(&file, (int32_t)(same_span_target - file.pos),
                FFFS_SEEK_CUR, &pos));
    ASSERT_TRUE(pos == same_span_target);
    ASSERT_TRUE(ffsv_flash_counts(flash)[FFSV_OP_READ].calls <=
            reads_before + 3u);
    ASSERT_OK(fffs_read(&file, out, 11, &nread));
    ASSERT_TRUE(nread == 11);
    ASSERT_TRUE(memcmp(out, large + same_span_target, nread) == 0);

    reads_before = ffsv_flash_counts(flash)[FFSV_OP_READ].calls;
    ASSERT_OK(fffs_seek(&file, 11, FFFS_SEEK_SET, &pos));
    ASSERT_TRUE(ffsv_flash_counts(flash)[FFSV_OP_READ].calls <=
            reads_before + 3u);
    ASSERT_TRUE(pos == 11);
    ASSERT_OK(fffs_read(&file, out, 19, &nread));
    ASSERT_TRUE(nread == 19);
    ASSERT_TRUE(memcmp(out, large + 11, nread) == 0);
    ASSERT_OK(fffs_close(&file));

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    free(large);
    return 0;
}

static int test_inspect_classifies_live_and_orphaned_metadata(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const char *old_value = "old";
    const char *new_value = "new";
    struct fffs_inspect_summary summary;
    FILE *dump;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "config", (const uint8_t *)old_value,
                strlen(old_value)));
    ASSERT_OK(write_chunks(&fs, "config", (const uint8_t *)new_value,
                strlen(new_value)));
    ASSERT_OK(fffs_inspect_check(&backend, &summary));
    ASSERT_TRUE(summary.live_entries == 1);
    ASSERT_TRUE(summary.live_entries_corrupt == 0);
    ASSERT_TRUE(summary.md_live == 1);
#if FFFS_LAZY_DELETE_TOMBSTONES
    ASSERT_TRUE(summary.md_obsolete_orphaned == 1);
#else
    ASSERT_TRUE(summary.md_tombstoned == 1);
    ASSERT_TRUE(summary.md_obsolete_orphaned == 0);
#endif
    ASSERT_TRUE(summary.md_corrupt == 0);
    dump = tmpfile();
    ASSERT_TRUE(dump != NULL);
    ASSERT_OK(fffs_inspect_dump(&backend, dump));
    fclose(dump);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_inspect_reports_corrupt_live_head(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    const char *value = "value";
    struct fffs_inspect_summary summary;
    uint8_t corrupt = 0;
    size_t md_offset;
    uint16_t head = 0;
    bool found = false;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "config", (const uint8_t *)value,
                strlen(value)));
    ASSERT_OK(fffs_index_head_for_slot(&fs, fffs_hash16("config"), &head,
                &found));
    ASSERT_TRUE(found);
    md_offset = (size_t)head * fs.sector_size + fs.sector_size -
        10 - 1;
    ASSERT_OK(flash_to_fs(ffsv_flash_corrupt(flash, md_offset, &corrupt,
                    sizeof(corrupt), FFSV_CALLSITE)));
    ASSERT_OK(fffs_inspect_check(&backend, &summary));
    ASSERT_TRUE(summary.live_entries == 1);
    ASSERT_TRUE(summary.live_entries_corrupt == 1);
    ASSERT_TRUE(summary.md_corrupt == 1);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_workload_generator_runs_deterministically(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static test_index_cache_t fs_index_heads[TEST_INDEX_CACHE_WORDS];
    struct fffs_workload_summary workload;
    struct fffs_inspect_summary inspect;

    ASSERT_OK(new_backend_with_size(&flash, &backend, 4096 * 128));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(fffs_workload_run(&fs, &(struct fffs_workload_options){
                .seed = 0x12345678u,
                .rounds = 48,
                .file_count = 12,
                .max_file_size = 32,
            }, &workload));
    ASSERT_TRUE(workload.writes > 0);
    ASSERT_TRUE(workload.reads > 0 || workload.lists > 0);
    ASSERT_OK(fffs_inspect_check(&backend, &inspect));
    ASSERT_TRUE(inspect.live_entries_corrupt == 0);
    ASSERT_TRUE(inspect.md_corrupt == 0);

    fffs_unmount(&fs);
    ffsv_flash_destroy(flash);
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_format_mount_write_read_remount();
    failures += test_partial_full_hint_does_not_invalidate_sector();
    failures += test_allocator_repairs_partial_full_hint_after_scan();
    failures += test_free_window_rejects_partially_erased_footer_without_scan();
    failures += test_mount_requires_scratch();
    failures += test_overwrite_delete_and_remount();
    failures += test_delete_tombstones_each_sector_in_contiguous_span();
    failures += test_fsinfo_refresh_and_cached_accounting();
    failures += test_reserved_hash_slots_are_skipped();
    failures += test_replay_skips_reused_stale_index_heads();
    failures += test_non_strict_mount_skips_invalid_active_index_tail();
    failures +=
        test_non_strict_mount_does_not_clobber_nonterminal_index_record();
    failures += test_gc_reclaims_unindexed_orphan_sector();
    failures += test_gc_erases_dirty_sector_with_erased_footer();
    failures += test_gc_tombstones_sector_with_invalid_unknown_md();
    failures +=
        test_gc_tombstones_sector_with_reachable_invalid_metadata_normally();
    failures += test_gc_skips_open_writer_dirty_root_sector();
    failures += test_gc_skips_multiple_open_writer_dirty_sectors();
    failures += test_gc_skips_open_writer_root_and_current_extents();
    failures += test_gc_skips_open_writer_middle_extent();
    failures += test_gc_skips_open_writer_deep_middle_extent();
    failures += test_gc_reclaims_failed_open_writer_after_remount();
    failures += test_gc_reclaims_obsolete_index_history();
    failures += test_alloc_failure_runs_gc_to_free_sector();
    failures += test_alloc_reservation_skips_other_open_writer();
    failures += test_alloc_reservation_released_on_close();
    failures += test_alloc_uses_owner_reservation_for_next_extent();
    failures += test_alloc_skips_invalid_reserved_candidate();
    failures += test_alloc_trims_other_reservations_under_pressure();
    failures += test_alloc_revokes_other_reservation_under_pressure();
    failures += test_full_alloc_map_mount_requires_storage();
    failures += test_streaming_write_forces_gc_without_reclaiming_self();
    failures += test_streaming_write_fails_after_gc_exhausts_reclaimable_space();
    failures += test_replay_evicts_stale_hash_collision_head();
    failures += test_hash_remove_repairs_cluster_across_home_bucket();
    failures += test_mount_uses_orphan_lookahead_for_serial_hint();
    failures += test_mount_discovers_sector_size();
    failures += test_mount_discovers_small_sector_size();
    failures += test_format_tiny_sector_wins_over_old_large_remnant();
    failures += test_format_erases_expanded_index_area();
    failures += test_index_rotates_when_active_sector_fills();
    failures += test_index_rotation_commits_header_before_tombstone();
    failures += test_mount_finishes_interrupted_index_compaction();
    failures += test_index_compaction_poweroff_after_each_flash_op();
    failures += test_index_rotation_preserves_sequence_with_spare();
    failures += test_index_independent_compaction_spills_and_commits_last();
    failures += test_index_compaction_mixed_history_metrics();
    failures += test_index_header_discovery_without_sector_zero();
    failures += test_uncommitted_index_header_is_not_discovered();
    failures += test_large_file_uses_noncontiguous_extents();
    failures += test_large_file_uses_contiguous_spans();
    failures += test_span_head_skips_contiguous_continuations();
    failures += test_root_alloc_skips_sector_with_continuation_record();
    failures += test_continuation_alloc_requires_empty_sector();
    failures += test_gc_compacts_root_only_sector_under_pressure();
    failures += test_compaction_preserves_open_reader_data();
    failures += test_open_writer_close_preserves_reused_slot_file();
    failures += test_compaction_window_consumes_reserved_sector();
    failures += test_replacement_refreshes_old_head_after_alloc_gc();
    failures += test_read_seek_single_extent();
    failures += test_read_seek_across_extents();
    failures += test_inspect_classifies_live_and_orphaned_metadata();
    failures += test_inspect_reports_corrupt_live_head();
    failures += test_workload_generator_runs_deterministically();
    if (failures) {
        fprintf(stderr, "%d fastffs tests failed\n", failures);
        return 1;
    }
    printf("fastffs tests passed\n");
    return 0;
}
