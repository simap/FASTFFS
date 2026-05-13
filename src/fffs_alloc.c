/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS core allocation policy: erased-space probing, allocation cursor
 * traversal, live-head checks, and allocation-pressure GC fallback.
 */

#include "fffs_internal.h"

#define FFFS_ERASED_CHECK_CHUNK 64

int fffs_flash_span_is_erased(struct fffs *fs, size_t offset, size_t size) {
    uint8_t fallback[FFFS_ERASED_CHECK_CHUNK];
    uint8_t *chunk = fs->scratch ? fs->scratch : fallback;
    size_t chunk_size = fs->scratch ? fs->scratch_size : sizeof(fallback);
    while (size > 0) {
        size_t n = size < chunk_size ? size : chunk_size;
        int err = fffs_flash_read(fs, offset, chunk, n);
        if (err != FFFS_OK) {
            return err;
        }
        if (chunk == fs->scratch) {
            fffs_scratch_bump(fs);
        }
        for (size_t i = 0; i < n; i++) {
            if (chunk[i] != 0xff) {
                return FFFS_ERR_NO_SPACE;
            }
        }
        offset += n;
        size -= n;
    }
    return FFFS_OK;
}

size_t fffs_next_data_sector(struct fffs *fs, size_t sector) {
    sector++;
    if (sector < fs->index_sectors || sector >= fs->sector_count) {
        return fs->index_sectors;
    }
    return sector;
}

static int find_free_sector_once(struct fffs *fs, uint16_t *sector) {
    if (fs->sector_count <= fs->index_sectors) {
        return FFFS_ERR_NO_SPACE;
    }

    size_t s = fs->alloc_cursor;
    if (s < fs->index_sectors || s >= fs->sector_count) {
        s = fs->index_sectors;
    }

    size_t data_sectors = fs->sector_count - fs->index_sectors;
    for (size_t checked = 0; checked < data_sectors; checked++) {
        if (fffs_index_sector_is_live_head(fs, s) ||
                fffs_sector_is_inflight(fs, (uint16_t)s)) {
            s = fffs_next_data_sector(fs, s);
            continue;
        }
        int err = fffs_flash_span_is_erased(fs, s * fs->sector_size,
                fs->sector_size);
        if (err == FFFS_OK) {
            *sector = (uint16_t)s;
            fs->alloc_cursor = fffs_next_data_sector(fs, s);
            return FFFS_OK;
        }
        if (err != FFFS_ERR_NO_SPACE) {
            return err;
        }
        s = fffs_next_data_sector(fs, s);
    }
    return FFFS_ERR_NO_SPACE;
}

#if FFFS_GC_ON_ALLOC_FAILURE
static int allocate_erased_gc_sector(struct fffs *fs, uint16_t *sector) {
    uint16_t erased_sector;
    int err = fffs_gc_until_erased(fs, &erased_sector);
    if (err != FFFS_OK) {
        return err;
    }
    if (fffs_index_sector_is_live_head(fs, erased_sector) ||
            fffs_sector_is_inflight(fs, erased_sector)) {
        return FFFS_ERR_CORRUPT;
    }
    err = fffs_flash_span_is_erased(fs,
            (size_t)erased_sector * fs->sector_size, fs->sector_size);
    if (err != FFFS_OK) {
        return err;
    }
    *sector = erased_sector;
    fs->alloc_cursor = fffs_next_data_sector(fs, erased_sector);
    return FFFS_OK;
}
#endif

int fffs_find_free_sector(struct fffs *fs, uint16_t *sector) {
    int err = find_free_sector_once(fs, sector);
#if FFFS_GC_ON_ALLOC_FAILURE
    if (err == FFFS_ERR_NO_SPACE && fs->sector_count > fs->index_sectors) {
        return allocate_erased_gc_sector(fs, sector);
    }
#endif
    return err;
}
