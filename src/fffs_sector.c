/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS sector footer offsets, encoding, validation, tombstones, and
 * fullness hints.
 */

#include "fffs_internal.h"

#include <string.h>

size_t fffs_max_file_data_size(const struct fffs *fs) {
    size_t raw = fs->sector_size - FFFS_SECTOR_FOOTER_SIZE -
        FFFS_MD_FILE_RECORD_SIZE;
    if (raw > UINT16_MAX) {
        raw = UINT16_MAX;
    }
    return raw - (raw % fs->backend.program_granule);
}

size_t fffs_sector_footer_offset(struct fffs *fs, uint16_t sector) {
    return (size_t)sector * fs->sector_size + fs->sector_size -
        FFFS_SECTOR_FOOTER_SIZE;
}

size_t fffs_sector_metadata_offset(struct fffs *fs, uint16_t sector) {
    return fffs_sector_footer_offset(fs, sector) - FFFS_MD_FILE_RECORD_SIZE;
}

int fffs_read_sector_footer(struct fffs *fs, uint16_t sector,
        uint32_t *serial) {
    if (sector < fs->index_sectors || sector >= fs->sector_count) {
        return FFFS_ERR_CORRUPT;
    }

    uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
    int err = fffs_flash_read(fs, fffs_sector_footer_offset(fs, sector),
            footer, sizeof(footer));
    if (err != FFFS_OK) {
        return err;
    }
    enum fffs_lifecycle_object_state footer_state =
        fffs_lifecycle_decode_footer(footer[5]);
    if (footer[4] != FFFS_SECTOR_TYPE_FILE ||
            memcmp(footer + 6, FFFS_SECTOR_MAGIC, 4) != 0) {
        return FFFS_ERR_NO_SPACE;
    }
    if (footer_state != FFFS_LIFECYCLE_OBJECT_LIVE &&
            footer_state != FFFS_LIFECYCLE_OBJECT_TOMBSTONED) {
        return FFFS_ERR_NO_SPACE;
    }
    if (serial) {
        *serial = fffs_load_le32(footer);
    }
    return FFFS_OK;
}

void fffs_encode_sector_footer(uint8_t footer[FFFS_SECTOR_FOOTER_SIZE],
        uint32_t serial, bool valid) {
    memset(footer, 0xff, FFFS_SECTOR_FOOTER_SIZE);
    fffs_store_le32(footer, serial);
    footer[4] = FFFS_SECTOR_TYPE_FILE;
    footer[5] = valid ? FFFS_SECTOR_FLAGS_VALID : 0xff;
    memcpy(footer + 6, FFFS_SECTOR_MAGIC, 4);
}

int fffs_write_sector_footer(struct fffs *fs, uint16_t sector,
        uint32_t serial) {
    uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
    fffs_encode_sector_footer(footer, serial, false);
    int err = fffs_flash_program_aligned(fs, fffs_sector_footer_offset(fs, sector),
            footer, sizeof(footer));
    if (err != FFFS_OK) {
        return err;
    }

    uint8_t state = FFFS_SECTOR_FLAGS_VALID;
    return fffs_flash_program_aligned(fs,
            fffs_sector_footer_offset(fs, sector) + 5u,
            &state, sizeof(state));
}

int fffs_tombstone_sector(struct fffs *fs, uint16_t sector) {
    uint8_t state[4] = {
        FFFS_SECTOR_TYPE_FILE,
        FFFS_SECTOR_FLAGS_TOMBSTONED,
        0xff,
        0xff,
    };
    return fffs_flash_program_aligned(fs,
            fffs_sector_footer_offset(fs, sector) + 4, state, sizeof(state));
}

int fffs_mark_sector_full(struct fffs *fs, uint16_t sector) {
    uint8_t state = FFFS_SECTOR_FLAGS_FULL;
    return fffs_flash_program_aligned(fs,
            fffs_sector_footer_offset(fs, sector) + 5, &state, sizeof(state));
}
