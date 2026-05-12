/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS core allocation and reclaim policy: erased-space probing, allocation
 * cursor traversal, live-head checks, and prototype garbage collection.
 */

#include "fffs_internal.h"

#define FFFS_ERASED_CHECK_CHUNK 64

static int flash_span_is_erased(struct fffs *fs, size_t offset, size_t size) {
    uint8_t fallback[FFFS_ERASED_CHECK_CHUNK];
    uint8_t *chunk = fs->scratch ? fs->scratch : fallback;
    size_t chunk_size = fs->scratch ? fs->scratch_size : sizeof(fallback);
    while (size > 0) {
        size_t n = size < chunk_size ? size : chunk_size;
        int err = fffs_flash_read(fs, offset, chunk, n);
        if (err != FFFS_OK) {
            return err;
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

static bool sector_is_live_head(struct fffs *fs, size_t sector) {
    for (size_t i = 0; i < fs->index_hash_table_size; i++) {
        uint16_t head = fs->index_heads[i];
        if (head == sector) {
            return true;
        }
    }
    return false;
}

static int sector_footer_state(struct fffs *fs, size_t sector,
        bool *erased, bool *tombstoned) {
    uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
    int err = fffs_flash_read(fs, fffs_sector_footer_offset(fs,
                (uint16_t)sector), footer, sizeof(footer));
    if (err != FFFS_OK) {
        return err;
    }
    *erased = true;
    for (size_t i = 0; i < sizeof(footer); i++) {
        if (footer[i] != 0xff) {
            *erased = false;
            break;
        }
    }
    *tombstoned = false;
    if (*erased) {
        return FFFS_OK;
    }
    if (footer[4] != FFFS_SECTOR_TYPE_MIXED ||
            (footer[5] != FFFS_SECTOR_FLAGS_VALID &&
             footer[5] != FFFS_SECTOR_FLAGS_TOMBSTONED) ||
            footer[6] != 0xff || footer[7] != 0xff ||
            footer[8] != 'F' || footer[9] != 'F' ||
            footer[10] != 'S' || footer[11] != 'D') {
        return FFFS_ERR_CORRUPT;
    }
    *tombstoned = footer[5] == FFFS_SECTOR_FLAGS_TOMBSTONED;
    return FFFS_OK;
}

static int program_sector_tombstone(struct fffs *fs, size_t sector) {
    uint8_t state[4] = {
        FFFS_SECTOR_TYPE_MIXED,
        FFFS_SECTOR_FLAGS_TOMBSTONED,
        0xff,
        0xff,
    };
    return fffs_flash_program_aligned(fs, fffs_sector_footer_offset(fs,
                (uint16_t)sector) + 4, state, sizeof(state));
}

static int sector_is_reachable_from_chain(struct fffs *fs, uint16_t head,
        size_t sector, bool *reachable) {
    uint16_t current = head;
    for (size_t depth = 0; current != 0 && depth < fs->sector_count; depth++) {
        if (current == sector) {
            *reachable = true;
            return FFFS_OK;
        }
        uint16_t next;
        int err = fffs_read_metadata(fs, current, NULL, NULL, NULL, NULL,
                &next);
        if (err != FFFS_OK) {
            return err;
        }
        current = next;
    }
    if (current != 0) {
        return FFFS_ERR_CORRUPT;
    }
    return FFFS_OK;
}

static int sector_is_live_extent(struct fffs *fs, size_t sector,
        bool *reachable) {
    *reachable = false;
    for (size_t i = 0; i < fs->index_hash_table_size; i++) {
        uint16_t head = fs->index_heads[i];
        if (head == 0) {
            continue;
        }
        int err = sector_is_reachable_from_chain(fs, head, sector,
                reachable);
        if (err != FFFS_OK || *reachable) {
            return err;
        }
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

int fffs_find_free_sector(struct fffs *fs, uint16_t *sector) {
    if (fs->sector_count <= fs->index_sectors) {
        return FFFS_ERR_NO_SPACE;
    }

    size_t s = fs->alloc_cursor;
    if (s < fs->index_sectors || s >= fs->sector_count) {
        s = fs->index_sectors;
    }

    size_t data_sectors = fs->sector_count - fs->index_sectors;
    for (size_t checked = 0; checked < data_sectors; checked++) {
        if (sector_is_live_head(fs, s)) {
            s = fffs_next_data_sector(fs, s);
            continue;
        }
        int err = flash_span_is_erased(fs, s * fs->sector_size,
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

int fffs_gc_step(struct fffs *fs, enum fffs_gc_action *out_action) {
    if (!fs) {
        return FFFS_ERR_INVALID;
    }
    if (out_action) {
        *out_action = FFFS_GC_IDLE;
    }
    if (fs->sector_count <= fs->index_sectors) {
        return FFFS_OK;
    }

    size_t s = fs->gc_cursor;
    if (s < fs->index_sectors || s >= fs->sector_count) {
        s = fs->index_sectors;
        fs->gc_live = false;
    }

    bool erased;
    bool tombstoned;
    int err = sector_footer_state(fs, s, &erased, &tombstoned);
    if (err != FFFS_OK) {
        return err;
    }
    if (erased) {
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_live = false;
        if (out_action) {
            *out_action = FFFS_GC_SCANNED;
        }
        return FFFS_OK;
    }
    if (tombstoned) {
        err = fffs_map_backend_status(fs->backend.erase(fs->backend.ctx,
                    s * fs->sector_size, fs->sector_size));
        if (err != FFFS_OK) {
            return err;
        }
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_live = false;
        if (out_action) {
            *out_action = FFFS_GC_ERASED;
        }
        return FFFS_OK;
    }

    bool live_extent = false;
    err = sector_is_live_extent(fs, s, &live_extent);
    if (err != FFFS_OK) {
        return err;
    }
    if (live_extent) {
        fs->gc_live = true;
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        if (out_action) {
            *out_action = FFFS_GC_SCANNED;
        }
        return FFFS_OK;
    }

    err = program_sector_tombstone(fs, s);
    if (err != FFFS_OK) {
        return err;
    }
    if (out_action) {
        *out_action = FFFS_GC_TOMBSTONED;
    }
    return FFFS_OK;
}

int fffs_gc(struct fffs *fs, size_t max_steps, size_t *out_erased) {
    if (!fs) {
        return FFFS_ERR_INVALID;
    }
    if (out_erased) {
        *out_erased = 0;
    }
    size_t erased = 0;
    for (size_t i = 0; i < max_steps; i++) {
        enum fffs_gc_action action;
        int err = fffs_gc_step(fs, &action);
        if (err != FFFS_OK) {
            return err;
        }
        if (action == FFFS_GC_ERASED) {
            erased += 1;
            if (out_erased) {
                *out_erased = erased;
            }
        }
    }
    return FFFS_OK;
}
