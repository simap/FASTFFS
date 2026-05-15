/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS garbage collection control: incremental sector reclaim steps,
 * bounded allocation-pressure reclaim, and public GC convenience loops.
 */

#include "fffs_internal.h"

enum gc_sector_state {
    GC_SECTOR_BLANK,
    GC_SECTOR_DIRTY_NO_FOOTER,
    GC_SECTOR_VALID,
    GC_SECTOR_TOMBSTONED,
};

static int sector_footer_state(struct fffs *fs, size_t sector,
        enum gc_sector_state *state) {
    uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
    int err = fffs_flash_read(fs, fffs_sector_footer_offset(fs,
                (uint16_t)sector), footer, sizeof(footer));
    if (err != FFFS_OK) {
        return err;
    }
    bool footer_erased = true;
    for (size_t i = 0; i < sizeof(footer); i++) {
        if (footer[i] != 0xff) {
            footer_erased = false;
            break;
        }
    }
    if (footer_erased) {
        err = fffs_flash_span_is_erased(fs, sector * fs->sector_size,
                fs->sector_size);
        if (err == FFFS_OK) {
            *state = GC_SECTOR_BLANK;
            return FFFS_OK;
        }
        if (err == FFFS_ERR_NO_SPACE) {
            *state = GC_SECTOR_DIRTY_NO_FOOTER;
            return FFFS_OK;
        }
        return err;
    }
    if (footer[4] == FFFS_SECTOR_TYPE_MIXED &&
            footer[5] == FFFS_SECTOR_FLAGS_TOMBSTONED &&
            footer[6] == 0xff && footer[7] == 0xff &&
            footer[8] == 'F' && footer[9] == 'F' &&
            footer[10] == 'S' && footer[11] == 'D') {
        *state = GC_SECTOR_TOMBSTONED;
        return FFFS_OK;
    }
    if (footer[4] != FFFS_SECTOR_TYPE_MIXED ||
            footer[5] != FFFS_SECTOR_FLAGS_VALID ||
            footer[6] != 0xff || footer[7] != 0xff ||
            footer[8] != 'F' || footer[9] != 'F' ||
            footer[10] != 'S' || footer[11] != 'D') {
        return FFFS_ERR_CORRUPT;
    }
    *state = GC_SECTOR_VALID;
    return FFFS_OK;
}

static size_t normalized_data_cursor(struct fffs *fs, size_t sector) {
    if (sector < fs->index_sectors || sector >= fs->sector_count) {
        return fs->index_sectors;
    }
    return sector;
}

static int sector_is_reachable_from_chain(struct fffs *fs, uint16_t head,
        size_t sector, bool *reachable) {
    uint16_t current = head;
    for (size_t depth = 0; current != 0 && depth < fs->sector_count; depth++) {
        if (current == sector) {
            *reachable = true;
            return FFFS_OK;
        }
        uint16_t next_sector;
        int err = fffs_read_metadata(fs, current, NULL, NULL, NULL, NULL,
                &next_sector);
        if (err != FFFS_OK) {
            return err;
        }
        current = next_sector;
    }
    if (current != 0) {
        return FFFS_ERR_CORRUPT;
    }
    return FFFS_OK;
}

static int gc_step(struct fffs *fs, enum fffs_gc_action *out_action,
        bool use_map) {
    if (!fs) {
        return FFFS_ERR_INVALID;
    }
    if (out_action) {
        *out_action = FFFS_GC_IDLE;
    }
    if (fs->sector_count <= fs->index_sectors) {
        return FFFS_OK;
    }

    size_t s = normalized_data_cursor(fs, fs->gc_cursor);
    if (s != fs->gc_cursor) {
        fs->gc_live = false;
    }
    if (use_map && fffs_alloc_map_maybe_used(fs, (uint16_t)s)) {
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_live = false;
        if (out_action) {
            *out_action = FFFS_GC_SCANNED;
        }
        return FFFS_OK;
    }
    if (fffs_sector_is_inflight(fs, (uint16_t)s)) {
        fffs_alloc_map_mark_used(fs, (uint16_t)s);
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_live = false;
        if (out_action) {
            *out_action = FFFS_GC_SCANNED;
        }
        return FFFS_OK;
    }

    enum gc_sector_state state;
    int err = sector_footer_state(fs, s, &state);
    if (err != FFFS_OK) {
        return err;
    }
    if (state == GC_SECTOR_BLANK) {
        fffs_alloc_map_mark_unknown(fs, (uint16_t)s);
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_live = false;
        if (out_action) {
            *out_action = FFFS_GC_SCANNED;
        }
        return FFFS_OK;
    }
    if (state == GC_SECTOR_TOMBSTONED ||
            state == GC_SECTOR_DIRTY_NO_FOOTER) {
        err = fffs_map_backend_status(fs->backend.erase(fs->backend.ctx,
                    s * fs->sector_size, fs->sector_size));
        if (err != FFFS_OK) {
            return err;
        }
        fffs_alloc_map_mark_unknown(fs, (uint16_t)s);
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_live = false;
        if (out_action) {
            *out_action = FFFS_GC_ERASED;
        }
        return FFFS_OK;
    }

    struct fffs_stat st;
    uint16_t md_slot = 0;
    uint16_t md_data_off;
    uint16_t md_data_len;
    uint16_t md_next;
    err = fffs_read_metadata(fs, (uint16_t)s, &st, &md_slot, &md_data_off,
            &md_data_len, &md_next);
    bool have_md = err == FFFS_OK;
    if (err != FFFS_OK && err != FFFS_ERR_CORRUPT) {
        return err;
    }
    if (have_md && fffs_name_is_inflight(fs, st.name)) {
        fffs_alloc_map_mark_used(fs, (uint16_t)s);
        fs->gc_live = true;
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        if (out_action) {
            *out_action = FFFS_GC_SCANNED;
        }
        return FFFS_OK;
    }
    (void)md_data_off;
    (void)md_data_len;
    (void)md_next;

    bool live_extent = false;
    if (have_md) {
        uint16_t head;
        bool found;
        err = fffs_index_head_for_slot(fs, md_slot, &head, &found);
        if (err != FFFS_OK) {
            return err;
        }
        if (!found) {
            live_extent = false;
        } else {
            err = sector_is_reachable_from_chain(fs, head, s,
                    &live_extent);
        }
        if (err != FFFS_OK) {
            return err;
        }
    }
    if (live_extent) {
        fffs_alloc_map_mark_used(fs, (uint16_t)s);
        fs->gc_live = true;
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        if (out_action) {
            *out_action = FFFS_GC_SCANNED;
        }
        return FFFS_OK;
    }

    err = fffs_tombstone_sector(fs, (uint16_t)s);
    if (err != FFFS_OK) {
        return err;
    }
    fffs_alloc_map_mark_unknown(fs, (uint16_t)s);
    if (out_action) {
        *out_action = FFFS_GC_TOMBSTONED;
    }
    return FFFS_OK;
}

int fffs_gc_step(struct fffs *fs, enum fffs_gc_action *out_action) {
    return gc_step(fs, out_action, true);
}

int fffs_gc_until_erased(struct fffs *fs, uint16_t *erased_sector) {
    if (!fs || !erased_sector) {
        return FFFS_ERR_INVALID;
    }
    if (fs->sector_count <= fs->index_sectors) {
        return FFFS_ERR_NO_SPACE;
    }

    size_t data_sectors = fs->sector_count - fs->index_sectors;
    for (size_t pass = 0; pass < 2; pass++) {
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_NONE
        if (pass != 0) {
            break;
        }
#endif
        bool use_map = pass == 0;
        size_t advanced = 0;
        while (advanced < data_sectors) {
            size_t before = normalized_data_cursor(fs, fs->gc_cursor);
            enum fffs_gc_action action;
            int err = gc_step(fs, &action, use_map);
            if (err != FFFS_OK) {
                return err;
            }
            if (action == FFFS_GC_ERASED) {
                *erased_sector = (uint16_t)before;
                return FFFS_OK;
            }

            size_t after = normalized_data_cursor(fs, fs->gc_cursor);
            if (after != before) {
                advanced += 1;
            } else if (action == FFFS_GC_IDLE) {
                break;
            }
        }
    }
    return FFFS_ERR_NO_SPACE;
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
