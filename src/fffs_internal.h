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

int fffs_flash_read(struct fffs *fs, size_t offset,
        void *buffer, size_t size);
int fffs_flash_program(struct fffs *fs, size_t offset,
        const void *buffer, size_t size);
int fffs_flash_program_aligned(struct fffs *fs, size_t offset,
        const void *buffer, size_t size);

bool fffs_valid_index_header(const uint8_t hdr[FFFS_HEADER_SIZE],
        uint8_t *index_sectors, uint8_t *sector_shift, uint8_t *serial);
int fffs_program_index_header(const struct fffs_backend *backend,
        size_t offset, uint8_t index_sectors, uint8_t sector_shift,
        uint8_t serial);
struct fffs_index_sequence {
    uint8_t index_sectors;
    uint8_t sector_shift;
    uint8_t active_serial;
    size_t active_sector;
    size_t oldest_sector;
    size_t count;
};
int fffs_find_index_sequence(const struct fffs_backend *backend,
        struct fffs_index_sequence *sequence);
int fffs_read_index_record(struct fffs *fs, size_t offset,
        uint16_t *slot, uint16_t *head);
int fffs_append_index_record(struct fffs *fs, uint16_t slot,
        uint16_t head);
int fffs_rotate_index(struct fffs *fs);
int fffs_replay_index(struct fffs *fs);
size_t fffs_max_file_data_size(const struct fffs *fs);
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

int fffs_index_candidate(struct fffs *fs, uint16_t slot, size_t probe,
        uint16_t *head, bool *end);
int fffs_index_insert(struct fffs *fs, uint16_t slot, uint16_t head);
int fffs_index_remove(struct fffs *fs, uint16_t slot);
int fffs_index_set(struct fffs *fs, uint16_t slot, uint16_t head);
bool fffs_index_hash_table_size_valid(size_t count);

size_t fffs_next_data_sector(struct fffs *fs, size_t sector);
int fffs_find_free_sector(struct fffs *fs, uint16_t *sector);

#endif
