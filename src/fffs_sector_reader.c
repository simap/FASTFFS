/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS sector-local buffered flash reader.
 */

#include "fffs_internal.h"

int fffs_sector_reader_view(struct fffs *fs,
        struct fffs_sector_reader *reader, uint16_t sector, size_t offset,
        size_t size, const uint8_t **out) {
    if (sector >= fs->sector_count || offset > fs->sector_size ||
            size > fs->sector_size - offset || size > reader->capacity) {
        return FFFS_ERR_INVALID;
    }
    if (reader->sector != sector) {
        reader->sector = sector;
        reader->start = 0;
        reader->len = 0;
    }
    if (reader->len != 0 && offset >= reader->start && size <= reader->len &&
            offset - reader->start <= reader->len - size) {
        *out = reader->data + (offset - reader->start);
        return FFFS_OK;
    }

    size_t start = offset;
    size_t len = reader->capacity;
    if (reader->reverse) {
        size_t end = offset + size;
        if (len > end) {
            len = end;
        }
        start = end - len;
    } else if (len > fs->sector_size - start) {
        len = fs->sector_size - start;
    }

    int err = fffs_flash_read(fs,
            (size_t)reader->sector * fs->sector_size + start,
            reader->data, len);
    if (err != FFFS_OK) {
        return err;
    }
    reader->start = start;
    reader->len = len;
    *out = reader->data + (offset - reader->start);
    return FFFS_OK;
}
