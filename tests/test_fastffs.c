#include "fastffs/fastffs.h"
#include "fastffs/fastffs_host.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

static int flash_to_fs(int status) {
    return status == FFSV_OK ? FFFS_OK : FFFS_ERR_IO;
}

static int new_backend(struct ffsv_flash **flash,
        struct fffs_backend *backend) {
    int err = ffsv_flash_create_with_preset(flash,
            FFSV_PRESET_GENERIC_NOR, 4096 * 16);
    if (err != FFSV_OK) {
        return FFFS_ERR_IO;
    }
    return fffs_host_backend_from_verify_flash(backend, *flash);
}

static int mount_fs(struct fffs *fs, const struct fffs_backend *backend,
        uint16_t *index_heads) {
    return fffs_mount(fs, backend, &(struct fffs_mount_options){
        .index_heads = index_heads,
        .index_head_count =
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
            FFFS_SLOT_COUNT,
#else
            FFFS_INDEX_HASH_HEAD_COUNT,
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
    static uint16_t fs_index_heads[
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
        FFFS_SLOT_COUNT
#else
        FFFS_INDEX_HASH_HEAD_COUNT
#endif
    ];
    static uint16_t remount_index_heads[
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
        FFFS_SLOT_COUNT
#else
        FFFS_INDEX_HASH_HEAD_COUNT
#endif
    ];
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
    static uint16_t fs_index_heads[
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
        FFFS_SLOT_COUNT
#else
        FFFS_INDEX_HASH_HEAD_COUNT
#endif
    ];
    static uint16_t remount_index_heads[
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
        FFFS_SLOT_COUNT
#else
        FFFS_INDEX_HASH_HEAD_COUNT
#endif
    ];
    const char *old_value = "old";
    const char *new_value = "new value";
    uint8_t out[32] = {0};
    size_t out_size = 0;
    struct fffs_stat st;

    ASSERT_OK(new_backend(&flash, &backend));
    ASSERT_OK(fffs_format(&backend, NULL));
    ASSERT_OK(mount_fs(&fs, &backend, fs_index_heads));
    ASSERT_OK(write_chunks(&fs, "config", (const uint8_t *)old_value,
                strlen(old_value)));
    ASSERT_OK(write_chunks(&fs, "config", (const uint8_t *)new_value,
                strlen(new_value)));
    ASSERT_OK(fffs_stat(&fs, "config", &st));
    ASSERT_TRUE(st.size == strlen(new_value));
    ASSERT_OK(read_chunks(&fs, "config", out, sizeof(out), &out_size));
    ASSERT_TRUE(out_size == strlen(new_value));
    ASSERT_TRUE(memcmp(out, new_value, out_size) == 0);
    ASSERT_OK(fffs_delete_file(&fs, "config"));
    ASSERT_EQ_INT(FFFS_ERR_NOT_FOUND, fffs_stat(&fs, "config", &st));
    fffs_unmount(&fs);

    ASSERT_OK(mount_fs(&remounted, &backend, remount_index_heads));
    ASSERT_EQ_INT(FFFS_ERR_NOT_FOUND, fffs_stat(&remounted, "config", &st));

    fffs_unmount(&remounted);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_reserved_hash_slots_are_skipped(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static uint16_t fs_index_heads[
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
        FFFS_SLOT_COUNT
#else
        FFFS_INDEX_HASH_HEAD_COUNT
#endif
    ];
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

static int test_index_header_discovery_without_sector_zero(void) {
    struct ffsv_flash *flash = NULL;
    struct fffs_backend backend;
    struct fffs fs;
    static uint16_t fs_index_heads[
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
        FFFS_SLOT_COUNT
#else
        FFFS_INDEX_HASH_HEAD_COUNT
#endif
    ];
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

int main(void) {
    int failures = 0;
    failures += test_format_mount_write_read_remount();
    failures += test_overwrite_delete_and_remount();
    failures += test_reserved_hash_slots_are_skipped();
    failures += test_index_header_discovery_without_sector_zero();
    if (failures) {
        fprintf(stderr, "%d fastffs tests failed\n", failures);
        return 1;
    }
    printf("fastffs tests passed\n");
    return 0;
}
