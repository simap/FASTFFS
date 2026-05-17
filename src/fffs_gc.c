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
    FFFS_PROFILE_PUSH(fs, FFFS_PROFILE_GC_FOOTER_STATE);
    int err = fffs_flash_read(fs, fffs_sector_footer_offset(fs,
                (uint16_t)sector), footer, sizeof(footer));
    FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC_FOOTER_STATE);
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
    enum fffs_lifecycle_object_state footer_state =
        fffs_lifecycle_decode_footer(footer[5]);
    if (footer[4] != FFFS_SECTOR_TYPE_FILE ||
            footer[6] != 0xff || footer[7] != 0xff ||
            footer[8] != 'F' || footer[9] != 'F' ||
            footer[10] != 'S' || footer[11] != 'D' ||
            footer_state == FFFS_LIFECYCLE_OBJECT_INVALID) {
        return FFFS_ERR_CORRUPT;
    }
    if (footer_state == FFFS_LIFECYCLE_OBJECT_TOMBSTONED &&
            footer[6] == 0xff && footer[7] == 0xff &&
            footer[8] == 'F' && footer[9] == 'F' &&
            footer[10] == 'S' && footer[11] == 'D') {
        *state = GC_SECTOR_TOMBSTONED;
        return FFFS_OK;
    }
    if (footer_state != FFFS_LIFECYCLE_OBJECT_LIVE) {
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

static int sector_is_reachable_from_chain(struct fffs *fs, uint16_t slot,
        uint16_t head, size_t sector, bool *reachable) {
    uint16_t current = head;
    FFFS_PROFILE_PUSH(fs, FFFS_PROFILE_GC_REACHABILITY);
    for (size_t depth = 0; current != 0 && depth < fs->sector_count; depth++) {
        if (current == sector) {
            *reachable = true;
            FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC_REACHABILITY);
            return FFFS_OK;
        }
        uint16_t next_sector;
        int err = fffs_read_metadata_for_slot(fs, current, slot, NULL, NULL,
                NULL, &next_sector, NULL);
        if (err != FFFS_OK) {
            FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC_REACHABILITY);
            return err;
        }
        current = next_sector;
    }
    if (current != 0) {
        FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC_REACHABILITY);
        return FFFS_ERR_CORRUPT;
    }
    FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC_REACHABILITY);
    return FFFS_OK;
}

static int gc_classify_record(struct fffs *fs, size_t sector,
        const struct fffs_md_record *record, bool *live) {
    if (fffs_lifecycle_decode_md(record->state) !=
            FFFS_LIFECYCLE_OBJECT_LIVE) {
        return FFFS_OK;
    }
    if (fffs_slot_is_inflight(fs, record->slot)) {
        *live = true;
        return FFFS_OK;
    }

    uint16_t head;
    bool found;
    int err = fffs_index_head_for_slot(fs, record->slot, &head, &found);
    if (err != FFFS_OK) {
        return err;
    }
    if (!found) {
        return FFFS_OK;
    }

    bool reachable = false;
    err = sector_is_reachable_from_chain(fs, record->slot, head, sector,
            &reachable);
    if (err != FFFS_OK) {
        return err;
    }
    if (reachable) {
        *live = true;
    } else {
        err = fffs_tombstone_metadata_for_slot(fs, (uint16_t)sector,
                record->slot);
    }
    return err;
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
        fs->gc_md_active = false;
    }
    if (use_map && fffs_alloc_map_maybe_used(fs, (uint16_t)s)) {
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_live = false;
        fs->gc_md_active = false;
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
    if (state == GC_SECTOR_DIRTY_NO_FOOTER &&
            fffs_sector_is_inflight(fs, (uint16_t)s)) {
        fffs_alloc_map_mark_used(fs, (uint16_t)s);
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_live = true;
        fs->gc_md_active = false;
        if (out_action) {
            *out_action = FFFS_GC_SCANNED;
        }
        return FFFS_OK;
    }
    if (state == GC_SECTOR_TOMBSTONED ||
            state == GC_SECTOR_DIRTY_NO_FOOTER) {
        err = fffs_map_backend_status(fs->backend.erase(fs->backend.ctx,
                    s * fs->sector_size, fs->sector_size));
#if FFFS_PROFILE_TRACE
        fffs_profile_flash(fs, FFFS_PROFILE_FLASH_ERASE,
                s * fs->sector_size, fs->sector_size);
#endif
        if (err != FFFS_OK) {
            return err;
        }
        fffs_alloc_map_mark_unknown(fs, (uint16_t)s);
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_live = false;
        fs->gc_md_active = false;
        if (out_action) {
            *out_action = FFFS_GC_ERASED;
        }
        return FFFS_OK;
    }

    if (!fs->gc_md_active || fs->gc_md_sector != s) {
        fs->gc_md_active = true;
        fs->gc_md_sector = (uint16_t)s;
        fs->gc_md_cursor = fs->sector_size - FFFS_SECTOR_FOOTER_SIZE;
        fs->gc_md_claimed_data_end = 0;
        fs->gc_live = false;
    }

    struct fffs_md_record record;
    size_t next_cursor = 0;
    bool erased = false;
    bool invalid = false;
    bool done = false;
    err = fffs_read_metadata_record_at(fs, (uint16_t)s, fs->gc_md_cursor,
            &fs->gc_md_claimed_data_end, &record, &next_cursor, &erased,
            &invalid, &done);
    if (err == FFFS_ERR_CORRUPT) {
        fs->gc_md_active = false;
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
    if (err != FFFS_OK) {
        fs->gc_md_active = false;
        return err;
    }
    if (invalid) {
        uint8_t tombstone = FFFS_MD_FLAGS_TOMBSTONED;
        err = fffs_flash_program_aligned(fs,
                s * fs->sector_size + record.record_start,
                &tombstone, sizeof(tombstone));
        fs->gc_md_active = false;
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fffs_alloc_map_mark_used(fs, (uint16_t)s);
        if (err != FFFS_OK) {
            return err;
        }
        if (out_action) {
            *out_action = FFFS_GC_TOMBSTONED;
        }
        return FFFS_OK;
    }
    if (!erased) {
        bool record_live = false;
        err = gc_classify_record(fs, s, &record, &record_live);
        if (err != FFFS_OK) {
            fs->gc_md_active = false;
            return err;
        }
        if (record_live) {
            fs->gc_live = true;
        }
        fs->gc_md_cursor = next_cursor;
    }

    if (!erased && !done) {
        if (out_action) {
            *out_action = fffs_lifecycle_decode_md(record.state) ==
                FFFS_LIFECYCLE_OBJECT_LIVE &&
                !fs->gc_live ? FFFS_GC_TOMBSTONED : FFFS_GC_SCANNED;
        }
        return FFFS_OK;
    }

    if (fs->gc_live) {
        fffs_alloc_map_mark_used(fs, (uint16_t)s);
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_md_active = false;
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
    fs->gc_md_active = false;
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
    FFFS_PROFILE_PUSH(fs, FFFS_PROFILE_GC);
    for (size_t i = 0; i < max_steps; i++) {
        enum fffs_gc_action action;
        int err = fffs_gc_step(fs, &action);
        if (err != FFFS_OK) {
            FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC);
            return err;
        }
        if (action == FFFS_GC_ERASED) {
            erased += 1;
            if (out_erased) {
                *out_erased = erased;
            }
        }
    }
    FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC);
    return FFFS_OK;
}
