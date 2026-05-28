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

void fffs_decode_sector_footer(const uint8_t footer[FFFS_SECTOR_FOOTER_SIZE],
        struct fffs_sector_footer *view) {
    *view = (struct fffs_sector_footer){
        .serial = fffs_load_le32(footer),
        .type = footer[4],
        .state = footer[5],
        .erased = fffs_flash_bytes_erased(footer, FFFS_SECTOR_FOOTER_SIZE),
        .magic_valid = memcmp(footer + 6, FFFS_SECTOR_MAGIC, 4) == 0,
        .valid_bits = fffs_lifecycle_valid_pair(footer[5]),
        .tombstone_bits = fffs_lifecycle_tombstone_pair(footer[5]),
        .full_bits = fffs_lifecycle_hint_pair(footer[5]),
    };
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
    struct fffs_sector_footer view;
    fffs_decode_sector_footer(footer, &view);
    if (view.type != FFFS_SECTOR_TYPE_FILE || !view.magic_valid) {
        return FFFS_ERR_NO_SPACE;
    }
    bool live = fffs_lifecycle_is_live(view.valid_bits, view.tombstone_bits);
    bool tombstoned = view.valid_bits != FFFS_BITMIRROR_MIXED &&
        view.tombstone_bits == FFFS_BITMIRROR_CLEARED;
    if (!live && !tombstoned) {
        return FFFS_ERR_NO_SPACE;
    }
    if (serial) {
        *serial = view.serial;
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
