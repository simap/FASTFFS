/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS private core declarations: shared on-flash constants, internal IO,
 * allocator, replay, and RAM-index interfaces used across core modules.
 */

#ifndef FASTFFS_FFFS_INTERNAL_H
#define FASTFFS_FFFS_INTERNAL_H

#include "fffs_bitmirror.h"
#include "fastffs/fastffs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct fffs_index_bucket {
    uint16_t slot;
    uint16_t head;
};

struct fffs_index_iter {
    struct fffs *fs;
    size_t pos;
    size_t cursor_seq_pos;
    size_t cursor_offset;
    int status;
};

struct fffs_read_cache_view {
    uint8_t *data;
    size_t capacity;
    size_t len;
    uint32_t data_pos;
};

#define FFFS_INDEX_MAGIC "FFFS"
#define FFFS_INDEX_VERSION 1
#define FFFS_HEADER_SIZE 8
#define FFFS_INDEX_FLAG_VALID 0x80
#define FFFS_INDEX_FLAG_TOMBSTONED 0x40
#define FFFS_INDEX_FLAG_MD_CRC_REQUIRED 0x20
#define FFFS_INDEX_FLAGS_VALID 0x7f
#define FFFS_MD_FILE_RECORD_SIZE 16
#define FFFS_MD_SIZE FFFS_MD_FILE_RECORD_SIZE
#define FFFS_MD_FLAG_VALID 0x80
#define FFFS_MD_FLAG_TOMBSTONED 0x40
#define FFFS_LIFECYCLE_VALID 0x7e
#define FFFS_LIFECYCLE_TOMBSTONED 0x3c
#define FFFS_LIFECYCLE_FULL 0x5a
#define FFFS_MD_FLAGS_VALID FFFS_LIFECYCLE_VALID
#define FFFS_MD_FLAGS_TOMBSTONED FFFS_LIFECYCLE_TOMBSTONED
#define FFFS_MD_TYPE_FILE_ROOT_V1 0x11
#define FFFS_MD_TYPE_FILE_CONT_V1 0x12
#define FFFS_SECTOR_FOOTER_SIZE 10
#define FFFS_SECTOR_MAGIC "FFSD"
#define FFFS_SECTOR_FLAG_VALID 0x80
#define FFFS_SECTOR_FLAG_TOMBSTONED 0x40
#define FFFS_SECTOR_FLAGS_VALID FFFS_LIFECYCLE_VALID
#define FFFS_SECTOR_FLAGS_TOMBSTONED FFFS_LIFECYCLE_TOMBSTONED
#define FFFS_SECTOR_FLAGS_FULL FFFS_LIFECYCLE_FULL
#define FFFS_SECTOR_TYPE_FILE 0x01
#define FFFS_SECTORS_PER_FILE_Q8_ONE 256u

#if FFFS_MD_PRELOAD_MAX < FFFS_MD_FILE_RECORD_SIZE + FFFS_SECTOR_FOOTER_SIZE
#error "FFFS_MD_PRELOAD_MAX must fit at least one metadata record and footer"
#endif

static inline uint16_t fffs_load_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t fffs_load_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void fffs_store_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void fffs_store_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline enum fffs_bitmirror_state fffs_lifecycle_valid_pair(
        uint8_t state) {
    return fffs_bitmirror_state(state, 0x81);
}

static inline enum fffs_bitmirror_state fffs_lifecycle_tombstone_pair(
        uint8_t state) {
    return fffs_bitmirror_state(state, 0x42);
}

static inline enum fffs_bitmirror_state fffs_lifecycle_hint_pair(
        uint8_t state) {
    return fffs_bitmirror_state(state, 0x24);
}

static inline bool fffs_lifecycle_is_live(
        enum fffs_bitmirror_state valid_bits,
        enum fffs_bitmirror_state tombstone_bits) {
    return valid_bits == FFFS_BITMIRROR_CLEARED &&
        tombstone_bits == FFFS_BITMIRROR_SET;
}

int fffs_map_backend_status(int status);
bool fffs_valid_backend(const struct fffs_backend *backend);
uint16_t fffs_hash16(const char *name);
uint16_t fffs_normalize_slot_base(uint16_t slot);
uint16_t fffs_next_slot(uint16_t slot);

bool fffs_flash_bytes_erased(const void *bytes, size_t size);
int fffs_flash_read(struct fffs *fs, size_t offset,
        void *buffer, size_t size);
int fffs_flash_program(struct fffs *fs, size_t offset,
        const void *buffer, size_t size);
int fffs_backend_program_aligned(const struct fffs_backend *backend,
        size_t offset, const void *buffer, size_t size);
int fffs_flash_program_aligned(struct fffs *fs, size_t offset,
        const void *buffer, size_t size);
void fffs_scratch_bump(struct fffs *fs);

#if FFFS_PROFILE_TRACE
void fffs_profile_push(struct fffs *fs, enum fffs_profile_scope scope);
void fffs_profile_pop(struct fffs *fs, enum fffs_profile_scope scope);
void fffs_profile_flash(struct fffs *fs, enum fffs_profile_flash_op op,
        size_t offset, size_t size);
#define FFFS_PROFILE_PUSH(fs, scope) fffs_profile_push((fs), (scope))
#define FFFS_PROFILE_POP(fs, scope) fffs_profile_pop((fs), (scope))
#else
#define FFFS_PROFILE_PUSH(fs, scope) ((void)0)
#define FFFS_PROFILE_POP(fs, scope) ((void)0)
#endif

/* Decode and validate an index-sector header. */
bool fffs_valid_index_header(const uint8_t hdr[FFFS_HEADER_SIZE],
        uint8_t *index_sectors, uint8_t *sector_shift, uint8_t *serial);
int fffs_program_index_header(const struct fffs_backend *backend,
        size_t offset, uint8_t index_sectors, uint8_t sector_shift,
        uint8_t serial);

/* Valid circular sequence of index sectors selected during mount discovery. */
struct fffs_index_sequence {
    uint8_t index_sectors;
    uint8_t sector_shift;
    uint8_t active_serial;
    size_t active_sector;
    size_t oldest_sector;
    size_t count;
};

/* Discover the newest valid index sequence and its geometry. */
int fffs_find_index_sequence(const struct fffs_backend *backend,
        struct fffs_index_sequence *sequence);
int fffs_read_index_record(struct fffs *fs, size_t offset,
        uint16_t *slot, uint16_t *head);

/* Append a namespace journal record and update the mounted index backend. */
int fffs_append_index_record(struct fffs *fs, uint16_t slot,
        uint16_t head);

/* Compact, finish, rotate, and replay the circular namespace journal. */
int fffs_index_compact_oldest(struct fffs *fs);
int fffs_index_finish_interrupted_compaction(struct fffs *fs);
int fffs_rotate_index(struct fffs *fs);
int fffs_replay_index(struct fffs *fs);
size_t fffs_max_file_data_size(const struct fffs *fs);

/* Sector footer and metadata helpers for FASTFFS-owned data sectors. */
struct fffs_sector_footer {
    uint32_t serial;
    uint8_t type;
    uint8_t state;
    bool erased;
    bool magic_valid;
    enum fffs_bitmirror_state valid_bits;
    enum fffs_bitmirror_state tombstone_bits;
    enum fffs_bitmirror_state full_bits;
};
void fffs_decode_sector_footer(const uint8_t footer[FFFS_SECTOR_FOOTER_SIZE],
        struct fffs_sector_footer *view);
int fffs_read_sector_serial(struct fffs *fs, uint16_t sector,
        uint32_t *serial);
int fffs_write_sector_footer(struct fffs *fs, uint16_t sector,
        uint32_t serial);
int fffs_tombstone_sector(struct fffs *fs, uint16_t sector);
int fffs_mark_sector_full(struct fffs *fs, uint16_t sector);
size_t fffs_sector_metadata_offset(struct fffs *fs, uint16_t sector);
size_t fffs_sector_footer_offset(struct fffs *fs, uint16_t sector);
void fffs_encode_sector_footer(uint8_t footer[FFFS_SECTOR_FOOTER_SIZE],
        uint32_t serial, bool valid);
enum fffs_tombstone_accounting {
    FFFS_TOMBSTONE_NO_ACCOUNTING,
    FFFS_TOMBSTONE_COMMITTED_DELETE,
};
int fffs_tombstone_metadata_for_slot(struct fffs *fs, uint16_t sector,
        uint16_t want_slot, enum fffs_tombstone_accounting accounting,
        bool *accounted);
void fffs_fsinfo_note_committed_delete(struct fffs *fs, uint32_t size);
int fffs_write_extent_metadata(struct fffs_file *file, uint16_t sector,
        uint16_t data_off, uint16_t record_off, bool write_footer,
        uint16_t data_len, uint16_t span_len, uint32_t total_size,
        uint16_t next, uint32_t file_offset, bool commit_index);
int fffs_begin_extent_metadata(struct fffs_file *file, uint16_t sector,
        uint16_t data_off, uint16_t record_off, bool write_footer,
        uint16_t data_len, uint32_t file_offset);
int fffs_finish_extent_metadata(struct fffs_file *file, uint16_t sector,
        uint16_t next, uint16_t span_len, uint32_t total_size,
        bool write_size, bool make_live, bool commit_index);
int fffs_finish_deferred_root_metadata(struct fffs_file *file);
int fffs_program_extent_metadata(struct fffs *fs, uint16_t sector,
        uint16_t slot, uint8_t type, uint16_t data_off, uint16_t record_off,
        bool write_footer, uint16_t data_len, uint16_t span_len,
        uint32_t size_or_offset, uint16_t next, bool live);
int fffs_find_sector_free_window(struct fffs *fs, uint16_t sector,
        uint16_t min_free, uint16_t reject_slot,
        bool normal_allocation, uint16_t *data_off,
        uint16_t *record_off, bool *needs_footer, uint16_t *md_records);
enum fffs_md_walk_result {
    FFFS_MD_WALK_RECORD,
    FFFS_MD_WALK_END_ERASED,
    FFFS_MD_WALK_END_CLAIMED_DATA,
    FFFS_MD_WALK_END_INVALID,
};
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
int fffs_read_file_root_md(struct fffs *fs, uint16_t sector,
        uint16_t want_slot, struct fffs_stat *st, uint16_t *data_off,
        uint16_t *data_len, uint16_t *next, uint16_t *span_len,
        uint32_t *root_size, struct fffs_read_cache_view *cache);
struct fffs_sector_reader {
    uint8_t *data;
    size_t capacity;
    uint16_t sector;
    size_t start;
    size_t len;
    bool reverse;
};
int fffs_sector_reader_view(struct fffs *fs,
        struct fffs_sector_reader *reader, uint16_t sector, size_t offset,
        size_t size, const uint8_t **out);
int fffs_md_walk_init(struct fffs *fs, struct fffs_md_walk *walk,
        uint16_t sector, struct fffs_sector_reader *window);
int fffs_md_walk_next(struct fffs *fs, struct fffs_md_walk *walk,
        struct fffs_sector_reader *window, struct fffs_md_record *record,
        enum fffs_md_walk_result *result);

void fffs_bitset_clear(uint32_t *words, size_t word_count);
bool fffs_bitset_get(const uint32_t *words, size_t bit);
void fffs_bitset_set(uint32_t *words, size_t bit);

/*
 * Allocation-map backend API.
 *
 * Implementations provide optional RAM hints for used/free sector tracking.
 * The map is not namespace authority: a "maybe used" result may conservatively
 * skip a sector, while allocation and GC still use flash state and metadata
 * classification for correctness.
 */
bool fffs_alloc_map_config_valid(size_t sector_count,
        const struct fffs_mount_options *opts);
int fffs_alloc_map_mount_init(struct fffs *fs,
        const struct fffs_mount_options *opts);
bool fffs_alloc_map_maybe_used(struct fffs *fs, uint16_t sector);
void fffs_alloc_map_mark_used(struct fffs *fs, uint16_t sector);
void fffs_alloc_map_mark_unknown(struct fffs *fs, uint16_t sector);
void fffs_alloc_map_mark_range_unknown(struct fffs *fs,
        uint16_t first, uint16_t count);

/*
 * Namespace index backend API.
 *
 * Implementations are selected by FFFS_INDEX_CACHE_MODE and expose the same
 * logical namespace operations over different storage strategies:
 *
 * - name resolution: filename -> resolved slot/current head,
 * - slot resolution: resolved slot -> current head,
 * - compaction currentness: historical (slot, head, position) -> live record,
 * - enumeration and replay mutation helpers.
 */
int fffs_index_insert(struct fffs *fs, uint16_t slot, uint16_t head);
int fffs_index_remove(struct fffs *fs, uint16_t slot);
int fffs_index_set(struct fffs *fs, uint16_t slot, uint16_t head);
bool fffs_index_cache_config_valid(size_t count);
size_t fffs_index_cache_required_size(size_t count);

/*
 * Resolve a filename through the bounded hash/probe namespace for public name
 * operations and create-slot choice:
 * resolve(fs, name) -> slot, head, found, optional root metadata fields.
 */
int fffs_index_resolve(struct fffs *fs, const char *name,
        uint16_t *slot, uint16_t *head, bool *found,
        struct fffs_stat *out_st, uint16_t *payload_data_off,
        uint16_t *payload_data_len, uint16_t *next,
        struct fffs_read_cache_view *cache);

/*
 * Resolve an already-known namespace slot:
 * head_for_slot(fs, slot) -> head, found.
 */
int fffs_index_head_for_slot(struct fffs *fs, uint16_t slot,
        uint16_t *head, bool *found);
bool fffs_index_dir_read(struct fffs_dir *dir, struct fffs_stat *st);
bool fffs_index_iter_read(struct fffs_index_iter *iter, uint16_t *slot,
        uint16_t *head);

/*
 * Test a historical index record during compaction:
 * record_is_current(fs, seq_pos, offset, slot, head) -> current.
 */
int fffs_index_record_is_current(struct fffs *fs,
        size_t seq_pos, size_t offset, uint16_t slot, uint16_t head,
        bool *current);

/* Inflight writer checks protect sectors for allocation and slots for GC. */
bool fffs_sector_is_inflight(struct fffs *fs, uint16_t sector);
bool fffs_slot_is_inflight(struct fffs *fs, uint16_t slot);
size_t fffs_next_data_sector(struct fffs *fs, size_t sector);
bool fffs_fsinfo_committed_valid(const struct fffs *fs);
void fffs_fsinfo_invalidate_committed(struct fffs *fs);
void fffs_fsinfo_note_committed_add(struct fffs *fs, uint32_t size);
bool fffs_invalidate_old_chain(struct fffs *fs, uint16_t slot,
        uint16_t head, uint16_t next, bool tombstone,
        bool *root_accounted);

/* Allocation and GC helpers for erased data sectors. */
int fffs_flash_span_is_erased(struct fffs *fs, size_t offset, size_t size);
int fffs_gc_until_erased(struct fffs *fs, uint16_t *erased_sector);
int fffs_alloc_next_sector(struct fffs_file *file, uint16_t *sector);
int fffs_alloc_find_compaction_root_window(struct fffs *fs,
        uint16_t source_sector, uint16_t slot, uint16_t data_len,
        uint16_t *sector, uint16_t *data_off, uint16_t *record_off,
        bool *needs_footer);
void fffs_alloc_release_reservation(struct fffs_file *file);

#endif
