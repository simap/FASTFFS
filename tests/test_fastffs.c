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

#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
#define TEST_INDEX_HASH_TABLE_SIZE FFFS_SLOT_COUNT
#else
#define TEST_INDEX_HASH_TABLE_SIZE FFFS_INDEX_HASH_TABLE_SIZE
#endif

uint16_t fffs_hash16(const char *name);

static int flash_to_fs(int status) {
    return status == FFSV_OK ? FFFS_OK : FFFS_ERR_IO;
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

static int mount_fs(struct fffs *fs, const struct fffs_backend *backend,
        uint16_t *index_heads) {
    return fffs_mount(fs, backend, &(struct fffs_mount_options){
        .index_heads = index_heads,
        .index_hash_table_size =
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
            TEST_INDEX_HASH_TABLE_SIZE,
#else
            FFFS_INDEX_HASH_TABLE_SIZE,
#endif
    });
}

static int write_chunks(struct fffs *fs, const char *name,
        const uint8_t *data, size_t size) {
    struct fffs_file file;
    size_t written = 0;
    ASSERT_OK(fffs_open(fs, &file, name,
                FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC));
    for (size_t off = 0; off < size; off += 3) {
        size_t n = size - off < 3 ? size - off : 3;
        ASSERT_OK(fffs_write(&file, data + off, n, &written));
        ASSERT_TRUE(written == n);
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
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
    static uint16_t remount_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
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
    ASSERT_TRUE(fs.alloc_cursor == fs.index_sectors + 4);
    ASSERT_TRUE(fs.next_sector_serial == 5);

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
    ASSERT_TRUE(remounted.alloc_cursor == remounted.index_sectors + 4);
    ASSERT_TRUE(remounted.next_sector_serial == 5);
    memset(out, 0, sizeof(out));
    ASSERT_OK(read_chunks(&remounted, "beta", out, sizeof(out), &out_size));
    ASSERT_TRUE(out_size == sizeof(beta));
    ASSERT_TRUE(memcmp(out, beta, sizeof(beta)) == 0);

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_overwrite_delete_and_remount(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
    static uint16_t remount_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
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
    ASSERT_TRUE(fs.alloc_cursor == fs.index_sectors + 1);
    ASSERT_TRUE(fs.next_sector_serial == 2);
    ASSERT_OK(write_chunks(&fs, "config", (const uint8_t *)new_value,
                strlen(new_value)));
    ASSERT_TRUE(fs.alloc_cursor == fs.index_sectors + 2);
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
    ASSERT_TRUE(summary.md_tombstoned == 1);
#endif
    fffs_unmount(&fs);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_TRUE(remounted.alloc_cursor == remounted.index_sectors + 2);
    ASSERT_TRUE(remounted.next_sector_serial == 3);
    ASSERT_EQ_INT(FFFS_ERR_NOT_FOUND, fffs_stat(&remounted, "config", &st));

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_reserved_hash_slots_are_skipped(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
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

static int test_gc_reclaims_unindexed_orphan_sector(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
    uint8_t footer[12] = {
        0x7b, 0x00, 0x00, 0x00,
        0x01, 0x7f, 0xff, 0xff,
        'F', 'F', 'S', 'D',
    };
    uint8_t check[12];
    enum fffs_gc_action action;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));

    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash,
                    10 * FFFS_DEFAULT_SECTOR_SIZE +
                    FFFS_DEFAULT_SECTOR_SIZE - sizeof(footer),
                    footer, sizeof(footer), FFSV_CALLSITE)));
    fs.gc_cursor = 10;
    ASSERT_OK(fffs_gc_step(&fs, &action));
    ASSERT_TRUE(action == FFFS_GC_TOMBSTONED);
    ASSERT_OK(flash_to_fs(ffsv_flash_read(flash,
                    10 * FFFS_DEFAULT_SECTOR_SIZE +
                    FFFS_DEFAULT_SECTOR_SIZE - sizeof(check),
                    check, sizeof(check), FFSV_CALLSITE)));
    ASSERT_TRUE(check[5] == 0x3f);
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

static int test_gc_reclaims_obsolete_index_history(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
    static uint16_t remount_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
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
    ASSERT_OK(fffs_gc_step(&fs, &action));
    ASSERT_TRUE(action == FFFS_GC_TOMBSTONED);
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

static int test_replay_evicts_stale_hash_collision_head(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
    static uint16_t remount_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
    const char *stale_name = "collision-38";
    const char *live_name = "collision-42";
    const char *stale_value = "stale";
    const char *live_value = "live";
    uint16_t stale_slot = fffs_hash16(stale_name);
    size_t stale_bucket = stale_slot & (TEST_INDEX_HASH_TABLE_SIZE - 1);
    uint8_t tombstone[4] = {0x01, 0x3f, 0xff, 0xff};
    uint8_t out[16] = {0};
    size_t out_size = 0;
    uint16_t stale_head;

#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS
    uint16_t live_slot = fffs_hash16(live_name);
    ASSERT_TRUE((stale_slot & (TEST_INDEX_HASH_TABLE_SIZE - 1)) ==
            (live_slot & (TEST_INDEX_HASH_TABLE_SIZE - 1)));
#endif

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, stale_name, (const uint8_t *)stale_value,
                strlen(stale_value)));
    stale_head = fs.index_heads[stale_bucket];
    ASSERT_TRUE(stale_head >= fs.index_sectors);
    ASSERT_OK(write_chunks(&fs, live_name, (const uint8_t *)live_value,
                strlen(live_value)));
    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash,
                    (size_t)stale_head * fs.sector_size + fs.sector_size -
                    8, tombstone, sizeof(tombstone), FFSV_CALLSITE)));
    fffs_unmount(&fs);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_OK(read_chunks(&remounted, live_name, out, sizeof(out),
                &out_size));
    ASSERT_TRUE(out_size == strlen(live_value));
    ASSERT_TRUE(memcmp(out, live_value, strlen(live_value)) == 0);

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_mount_uses_orphan_lookahead_for_serial_hint(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    struct fffs remounted;
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
    static uint16_t remount_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
    const char *value = "committed";
    uint8_t orphan_footer[12] = {
        0x2c, 0x01, 0x00, 0x00,
        0x01, 0x7f, 0xff, 0xff,
        'F', 'F', 'S', 'D',
    };
    uint16_t orphan_sector;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "config", (const uint8_t *)value,
                strlen(value)));
    orphan_sector = (uint16_t)fs.alloc_cursor;
    ASSERT_OK(flash_to_fs(ffsv_flash_program(flash,
                    (size_t)orphan_sector * FFFS_DEFAULT_SECTOR_SIZE +
                    FFFS_DEFAULT_SECTOR_SIZE - sizeof(orphan_footer),
                    orphan_footer, sizeof(orphan_footer), FFSV_CALLSITE)));
    fffs_unmount(&fs);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_TRUE(remounted.alloc_cursor == orphan_sector);
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
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
    static uint16_t remount_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
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
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
    static uint16_t remount_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
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
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];

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
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];

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
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
    static uint16_t remount_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
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

static int test_index_header_discovery_without_sector_zero(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
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
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
    static uint16_t remount_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
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
    ASSERT_TRUE(fs.alloc_cursor == fs.index_sectors + 3);
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

static int test_inspect_classifies_live_and_orphaned_metadata(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
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
    ASSERT_TRUE(summary.md_obsolete_orphaned == 1);
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
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
    const char *value = "value";
    struct fffs_inspect_summary summary;
    uint8_t corrupt = 0;
    size_t md_offset;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "config", (const uint8_t *)value,
                strlen(value)));
    md_offset = fs.index_sectors * fs.sector_size + fs.sector_size -
        12 - 64;
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
    static uint16_t fs_index_heads[TEST_INDEX_HASH_TABLE_SIZE];
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
    failures += test_overwrite_delete_and_remount();
    failures += test_reserved_hash_slots_are_skipped();
    failures += test_gc_reclaims_unindexed_orphan_sector();
    failures += test_gc_reclaims_obsolete_index_history();
    failures += test_replay_evicts_stale_hash_collision_head();
    failures += test_mount_uses_orphan_lookahead_for_serial_hint();
    failures += test_mount_discovers_sector_size();
    failures += test_mount_discovers_small_sector_size();
    failures += test_format_tiny_sector_wins_over_old_large_remnant();
    failures += test_format_erases_expanded_index_area();
    failures += test_index_rotates_when_active_sector_fills();
    failures += test_index_header_discovery_without_sector_zero();
    failures += test_large_file_uses_noncontiguous_extents();
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
