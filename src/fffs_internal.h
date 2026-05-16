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

#include "fastffs/fastffs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct fffs_index_bucket {
    uint16_t slot;
    uint16_t head;
};

#define FFFS_INDEX_MAGIC "FFFS"
#define FFFS_INDEX_VERSION 1
#define FFFS_HEADER_SIZE 8
#define FFFS_INDEX_FLAG_VALID 0x80
#define FFFS_INDEX_FLAG_TOMBSTONED 0x40
#define FFFS_INDEX_FLAG_MD_CRC_REQUIRED 0x20
#define FFFS_INDEX_FLAGS_VALID 0x7f
#define FFFS_MD_SIZE 64
#define FFFS_MD_FLAG_VALID 0x80
#define FFFS_MD_FLAG_TOMBSTONED 0x40
#define FFFS_MD_FLAGS_VALID 0x7f
#define FFFS_MD_FLAGS_TOMBSTONED 0x3f
#define FFFS_MD_TYPE_BASELINE 0x01
#define FFFS_SECTOR_FOOTER_SIZE 12
#define FFFS_SECTOR_MAGIC "FFSD"
#define FFFS_SECTOR_FLAG_VALID 0x80
#define FFFS_SECTOR_FLAG_TOMBSTONED 0x40
#define FFFS_SECTOR_FLAGS_VALID 0x7f
#define FFFS_SECTOR_FLAGS_TOMBSTONED 0x3f
#define FFFS_SECTOR_TYPE_MIXED 0x01

int fffs_map_backend_status(int status);
bool fffs_valid_backend(const struct fffs_backend *backend);
uint16_t fffs_hash16(const char *name);
uint16_t fffs_normalize_slot_base(uint16_t slot);
uint16_t fffs_next_slot(uint16_t slot);

int fffs_flash_read(struct fffs *fs, size_t offset,
        void *buffer, size_t size);
int fffs_flash_program(struct fffs *fs, size_t offset,
        const void *buffer, size_t size);
int fffs_flash_program_aligned(struct fffs *fs, size_t offset,
        const void *buffer, size_t size);
void fffs_scratch_bump(struct fffs *fs);

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
int fffs_read_sector_footer(struct fffs *fs, uint16_t sector,
        uint32_t *serial);
int fffs_write_sector_footer(struct fffs *fs, uint16_t sector,
        uint32_t serial);
int fffs_tombstone_sector(struct fffs *fs, uint16_t sector);
size_t fffs_sector_metadata_offset(struct fffs *fs, uint16_t sector);
size_t fffs_sector_footer_offset(struct fffs *fs, uint16_t sector);
int fffs_read_metadata(struct fffs *fs, uint16_t sector,
        struct fffs_stat *st, uint16_t *slot, uint16_t *data_off,
        uint16_t *data_len, uint16_t *next);
int fffs_write_extent_metadata(struct fffs_file *file, uint16_t sector,
        uint32_t serial, uint16_t data_len, uint32_t total_size,
        uint16_t next, bool commit_index);

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
        struct fffs_stat *out_st, uint16_t *data_off, uint16_t *data_len,
        uint16_t *next);

/*
 * Resolve an already-known namespace slot:
 * head_for_slot(fs, slot) -> head, found.
 */
int fffs_index_head_for_slot(struct fffs *fs, uint16_t slot,
        uint16_t *head, bool *found);
bool fffs_index_dir_read(struct fffs_dir *dir, struct fffs_stat *st);

/*
 * Test a historical index record during compaction:
 * record_is_current(fs, seq_pos, offset, slot, head) -> current.
 */
int fffs_index_record_is_current(struct fffs *fs,
        size_t seq_pos, size_t offset, uint16_t slot, uint16_t head,
        bool *current);

/* Seed the allocation map with current live root heads after mount replay. */
void fffs_index_mark_live_heads_used(struct fffs *fs);

/* Inflight writer checks protect sectors and names not yet in the index. */
bool fffs_sector_is_inflight(struct fffs *fs, uint16_t sector);
bool fffs_name_is_inflight(struct fffs *fs, const char *name);
size_t fffs_next_data_sector(struct fffs *fs, size_t sector);

/* Allocation and GC helpers for erased data sectors. */
int fffs_flash_span_is_erased(struct fffs *fs, size_t offset, size_t size);
int fffs_gc_until_erased(struct fffs *fs, uint16_t *erased_sector);
int fffs_alloc_next_sector(struct fffs_file *file, uint16_t *sector);
void fffs_alloc_release_reservation(struct fffs_file *file);

#endif
