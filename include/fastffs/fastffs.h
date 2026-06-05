/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS public core API: caller-owned filesystem, file, directory, backend,
 * and status types for the portable embedded filesystem core.
 */

#ifndef FASTFFS_FASTFFS_H
#define FASTFFS_FASTFFS_H

#include "fastffs/fffs_opts.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum fffs_status {
    FFFS_OK = 0,
    FFFS_ERR_INVALID = -1,
    FFFS_ERR_NOMEM = -2,
    FFFS_ERR_RANGE = -3,
    FFFS_ERR_NO_SPACE = -4,
    FFFS_ERR_NOT_FOUND = -5,
    FFFS_ERR_EXISTS = -6,
    FFFS_ERR_NAME_TOO_LONG = -7,
    FFFS_ERR_CORRUPT = -8, //this is reserved for actual corruption, defined as: loss of previously successfully committed filesystem data/metadata"
    FFFS_ERR_IO = -9,
};

enum fffs_open_flags {
    FFFS_O_RDONLY = 0x01,
    FFFS_O_WRONLY = 0x02,
    FFFS_O_CREATE = 0x04,
    FFFS_O_TRUNC = 0x08,
    FFFS_O_EXCL = 0x10,
};

enum fffs_seek_whence {
    FFFS_SEEK_SET = 0,
    FFFS_SEEK_CUR = 1,
    FFFS_SEEK_END = 2,
};

enum fffs_gc_action {
    FFFS_GC_IDLE = 0,
    FFFS_GC_SCANNED = 1,
    FFFS_GC_TOMBSTONED = 2,
    FFFS_GC_ERASED = 3,
};

enum fffs_fsinfo_flags {
    FFFS_FSINFO_FAST = 0,
    FFFS_FSINFO_REFRESH_COMMITTED = 0x01,
    FFFS_FSINFO_ESTIMATE_METADATA = 0x02,
    FFFS_FSINFO_REFRESH_IF_NEEDED = 0x04,
};

enum fffs_fsinfo_valid_flags {
    FFFS_FSINFO_TOTAL_VALID = 0x01,
    FFFS_FSINFO_COMMITTED_FILES_VALID = 0x02,
    FFFS_FSINFO_COMMITTED_BYTES_VALID = 0x04,
    FFFS_FSINFO_INFLIGHT_VALID = 0x08,
    FFFS_FSINFO_METADATA_ESTIMATE_VALID = 0x10,
};

enum fffs_sector_size {
    FFFS_SECTOR_DEFAULT = 0,
    FFFS_SECTOR_256 = 256,
    FFFS_SECTOR_512 = 512,
    FFFS_SECTOR_1K = 1024,
    FFFS_SECTOR_2K = 2048,
    FFFS_SECTOR_4K = 4096,
    FFFS_SECTOR_8K = 8192,
};

enum {
    FFFS_DEFAULT_SECTOR_SIZE = FFFS_SECTOR_4K,
    FFFS_DEFAULT_SECTOR_SHIFT = 4,
    FFFS_MIN_SECTOR_SHIFT = 0,
    FFFS_MAX_SECTOR_SHIFT = 15,
    FFFS_DEFAULT_INDEX_SECTORS = 2,
    FFFS_MAX_NAME = 32,
    FFFS_MAX_PROBE_DISTANCE = 50,
    FFFS_SLOT_COUNT = 65536,
};

#ifndef FFFS_PROFILE_TRACE
#define FFFS_PROFILE_TRACE 0
#endif

#if FFFS_PROFILE_TRACE
enum fffs_profile_scope {
    FFFS_PROFILE_UNSCOPED = 0,
    FFFS_PROFILE_MOUNT,
    FFFS_PROFILE_INDEX_REPLAY,
    FFFS_PROFILE_INDEX_RESOLVE,
    FFFS_PROFILE_DIR_READ,
    FFFS_PROFILE_READ_METADATA,
    FFFS_PROFILE_SPAN_IS_ERASED,
    FFFS_PROFILE_ALLOC_NEXT_SECTOR,
    FFFS_PROFILE_GC,
    FFFS_PROFILE_GC_STEP,
    FFFS_PROFILE_GC_FOOTER_STATE,
    FFFS_PROFILE_GC_REACHABILITY,
    FFFS_PROFILE_READ,
    FFFS_PROFILE_WRITE,
    FFFS_PROFILE_CLOSE,
    FFFS_PROFILE_DELETE,
    FFFS_PROFILE_COUNT,
};

enum fffs_profile_flash_op {
    FFFS_PROFILE_FLASH_READ = 0,
    FFFS_PROFILE_FLASH_PROGRAM = 1,
    FFFS_PROFILE_FLASH_ERASE = 2,
};

struct fffs;
typedef void (*fffs_profile_trace_fn)(struct fffs *fs,
        const enum fffs_profile_scope *scope_stack, size_t scope_depth,
        enum fffs_profile_flash_op op, size_t offset, size_t size,
        void *user);
#endif

struct fffs_backend {
    void *ctx;
    size_t size;
    size_t read_granule;
    size_t program_granule;
    int (*read)(void *ctx, size_t offset, void *buffer, size_t size);
    int (*program)(void *ctx, size_t offset, const void *buffer, size_t size);
    int (*erase)(void *ctx, size_t offset, size_t size);
};

struct fffs_format_options {
    uint8_t index_sectors;
    enum fffs_sector_size sector_size;
};

struct fffs_mount_options {
    /* Caller-owned namespace cache storage. Size with FFFS_INDEX_CACHE_BYTES. */
    void *index_cache;
    size_t index_cache_size;
    size_t index_hash_table_size;
    void *scratch;
    size_t scratch_size;
    bool strict;
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    uint32_t *alloc_map;
    size_t alloc_map_words;
#endif
#if FFFS_PROFILE_TRACE
    fffs_profile_trace_fn profile_trace;
    void *profile_trace_user;
#endif
};

struct fffs_fsinfo {
    uint32_t valid_flags;
    uint32_t total_bytes;
    uint32_t committed_file_count;
    uint32_t committed_data_bytes;
    uint32_t inflight_file_count;
    uint32_t inflight_data_bytes;
    uint32_t estimated_metadata_bytes;
};

struct fffs_md_walk {
    size_t cursor;
    size_t claimed_data_end;
    uint32_t live_bytes;
    uint16_t sector;
    uint16_t live_root_count;
    bool live_seen;
    bool live_root_seen;
    bool live_continuation_seen;
    bool inflight_seen;
    bool active;
};

struct fffs_compaction_candidate {
    uint16_t sector;
    uint16_t trapped_reclaimable;
    uint16_t live_root_count;
};

struct fffs {
    struct fffs_backend backend;
    struct fffs_file *inflight_writers;
    void *index_cache;
    uint16_t *index_heads;
    size_t index_hash_table_size;
#if FFFS_PROFILE_TRACE
    fffs_profile_trace_fn profile_trace;
    void *profile_trace_user;
    enum fffs_profile_scope profile_stack[12];
    size_t profile_depth;
#endif
    uint8_t *scratch;
    size_t scratch_size;
    uint32_t scratch_serial;
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    uint32_t *alloc_map;
    size_t alloc_map_words;
#endif
    size_t sector_size;
    size_t sector_count;
    uint8_t sector_shift;
    uint8_t index_sectors;
    size_t oldest_index_sector;
    size_t index_sequence_count;
    size_t active_index_sector;
    size_t next_index_offset;
    uint8_t active_index_serial;
    size_t alloc_cursor;
    size_t gc_cursor;
    uint16_t compaction_reserve_count;
    uint16_t compaction_reserve_sectors[
        FFFS_COMPACTION_RESERVE_SECTORS > 0 ?
            FFFS_COMPACTION_RESERVE_SECTORS : 1];
    struct fffs_md_walk gc_md;
    struct fffs_compaction_candidate
        compaction_candidates[FFFS_COMPACTION_CANDIDATE_COUNT];
    bool strict;
    uint32_t next_sector_serial;
    uint32_t fsinfo_valid_flags;
    uint32_t committed_file_count;
    uint32_t committed_data_bytes;
    uint16_t avg_sectors_per_file_q8;
};

struct fffs_stat {
    char name[FFFS_MAX_NAME + 1];
    uint32_t size;
};

typedef struct fffs_stat fffs_dirent;

struct fffs_file {
    struct fffs *fs;
    struct fffs_file *inflight_next;
    uint32_t flags;
    uint16_t slot;
    uint16_t head;
    uint16_t current;
    uint16_t data_offset;
    uint16_t current_data_len;
    uint16_t current_next;
    uint16_t current_write_offset;
    uint16_t current_metadata_offset;
    uint16_t root_payload_offset;
    uint16_t span_head;
    uint16_t span_len;
    uint16_t old_head;
    uint16_t old_next;
    uint16_t reserve_first;
    uint16_t reserve_count;
    uint32_t size;
    uint32_t pos;
    uint32_t current_data_pos;
    uint32_t cache_data_pos;
    uint32_t current_file_offset;
    size_t cache_len;
    char name[FFFS_MAX_NAME + 1];
    uint8_t cache[FFFS_FILE_CACHE_SIZE];
    bool current_needs_footer;
    bool closed;
    bool inflight_registered;
};

struct fffs_dir {
    struct fffs *fs;
    size_t pos;
    size_t cursor_seq_pos;
    size_t cursor_offset;
    uint32_t scratch_serial;
    size_t cached_seq_pos;
    size_t cached_offset;
    size_t cached_len;
    size_t prefix_len;
    int status;
    char prefix[FFFS_MAX_NAME + 1];
    bool closed;
};

int fffs_format(const struct fffs_backend *backend,
        const struct fffs_format_options *options);
int fffs_mount(struct fffs *fs, const struct fffs_backend *backend,
        const struct fffs_mount_options *options);
void fffs_unmount(struct fffs *fs);

int fffs_open(struct fffs *fs, struct fffs_file *file,
        const char *name, uint32_t flags);
int fffs_close(struct fffs_file *file);
int fffs_read(struct fffs_file *file, void *buffer, size_t size,
        size_t *out_read);
int fffs_seek(struct fffs_file *file, int32_t offset,
        enum fffs_seek_whence whence, uint32_t *out_pos);
int fffs_write(struct fffs_file *file, const void *buffer, size_t size);
int fffs_fstat(struct fffs_file *file, struct fffs_stat *st);

int fffs_stat(struct fffs *fs, const char *name, struct fffs_stat *st);
int fffs_exists(struct fffs *fs, const char *name, bool *exists);
int fffs_delete_file(struct fffs *fs, const char *name);
int fffs_fsinfo(struct fffs *fs, struct fffs_fsinfo *info, uint32_t flags);
int fffs_gc_step(struct fffs *fs, enum fffs_gc_action *out_action);
int fffs_gc(struct fffs *fs, size_t max_steps, size_t *out_erased);
int fffs_dir_open(struct fffs *fs, struct fffs_dir *dir,
        const char *prefix);
bool fffs_dir_read(struct fffs_dir *dir, struct fffs_stat *st);
int fffs_dir_status(const struct fffs_dir *dir);
int fffs_dir_close(struct fffs_dir *dir);
int fffs_list(struct fffs *fs, struct fffs_stat *entries,
        size_t capacity, size_t *out_count);

const char *fffs_status_name(int status);

#ifdef __cplusplus
}
#endif

#endif
