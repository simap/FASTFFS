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
#define FFFS_SECTOR_FLAGS_VALID 0x7f
#define FFFS_SECTOR_TYPE_MIXED 0x01

int fffs_map_backend_status(int status);
bool fffs_valid_backend(const struct fffs_backend *backend);
uint16_t fffs_hash16(const char *name);

int fffs_flash_read(struct fffs *fs, size_t offset,
        void *buffer, size_t size);
int fffs_flash_program(struct fffs *fs, size_t offset,
        const void *buffer, size_t size);

bool fffs_valid_index_header(const uint8_t hdr[FFFS_HEADER_SIZE],
        uint8_t *index_sectors, uint8_t *sector_shift, uint8_t *serial);
int fffs_program_index_header(const struct fffs_backend *backend,
        size_t offset, uint8_t index_sectors, uint8_t sector_shift,
        uint8_t serial);
int fffs_find_active_index_header(const struct fffs_backend *backend,
        size_t *active, uint8_t *index_sectors, uint8_t *sector_shift,
        uint8_t *serial);
int fffs_read_index_record(struct fffs *fs, size_t offset,
        uint16_t *slot, uint16_t *head);
int fffs_append_index_record(struct fffs *fs, uint16_t slot,
        uint16_t head);
int fffs_rotate_index(struct fffs *fs);
int fffs_replay_index(struct fffs *fs);
size_t fffs_max_file_data_size(const struct fffs *fs);
int fffs_read_sector_footer(struct fffs *fs, uint16_t sector,
        uint32_t *serial);
int fffs_write_sector_footer(struct fffs_file *file);
int fffs_read_metadata(struct fffs *fs, uint16_t sector,
        struct fffs_stat *st, uint16_t *slot, uint16_t *data_off,
        uint16_t *data_len);
int fffs_write_root_metadata(struct fffs_file *file);

int fffs_index_find(struct fffs *fs, uint16_t slot, uint16_t *head);
int fffs_index_insert(struct fffs *fs, uint16_t slot, uint16_t head);
int fffs_index_remove(struct fffs *fs, uint16_t slot);
int fffs_index_set(struct fffs *fs, uint16_t slot, uint16_t head);
bool fffs_index_head_count_valid(size_t count);

size_t fffs_next_data_sector(struct fffs *fs, size_t sector);
int fffs_find_free_sector(struct fffs *fs, uint16_t *sector);

#endif
