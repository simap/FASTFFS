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
        enum gc_sector_state *state, bool *full_hint) {
    uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
    *full_hint = false;
    FFFS_PROFILE_PUSH(fs, FFFS_PROFILE_GC_FOOTER_STATE);
    int err = fffs_flash_read(fs, fffs_sector_footer_offset(fs,
                (uint16_t)sector), footer, sizeof(footer));
    FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC_FOOTER_STATE);
    if (err != FFFS_OK) {
        return err;
    }
    if (fffs_flash_bytes_erased(footer, sizeof(footer))) {
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
            footer[6] != 'F' || footer[7] != 'F' ||
            footer[8] != 'S' || footer[9] != 'D' ||
            footer_state == FFFS_LIFECYCLE_OBJECT_INVALID ||
            footer_state == FFFS_LIFECYCLE_OBJECT_ERASED) {
        *state = GC_SECTOR_DIRTY_NO_FOOTER;
        return FFFS_OK;
    }
    if (footer_state == FFFS_LIFECYCLE_OBJECT_TOMBSTONED) {
        *state = GC_SECTOR_TOMBSTONED;
        return FFFS_OK;
    }
    if (footer_state != FFFS_LIFECYCLE_OBJECT_LIVE) {
        return FFFS_ERR_CORRUPT;
    }
    *full_hint = fffs_lifecycle_hint_pair(footer[5]) ==
        FFFS_BITMIRROR_CLEARED;
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
        uint16_t next_sector;
        uint16_t span_len;
        int err = fffs_read_metadata_for_slot(fs, current, slot, NULL, NULL,
                NULL, &next_sector, &span_len, NULL, NULL);
        if (err != FFFS_OK) {
            FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC_REACHABILITY);
            return err;
        }
        size_t span_end = (size_t)current + span_len;
        if (span_end > fs->sector_count) {
            FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC_REACHABILITY);
            return FFFS_ERR_CORRUPT;
        }
        if (sector >= current && sector < span_end) {
            *reachable = true;
            FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC_REACHABILITY);
            return FFFS_OK;
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

static int sector_is_reachable_from_any_chain(struct fffs *fs, size_t sector,
        bool *reachable) {
    *reachable = false;
    struct fffs_index_iter iter = {
        .fs = fs,
    };
    uint16_t slot;
    uint16_t head;
    while (fffs_index_iter_read(&iter, &slot, &head)) {
        int err = sector_is_reachable_from_chain(fs, slot, head, sector,
                reachable);
        if (err != FFFS_OK || *reachable) {
            return err;
        }
    }
    return iter.status;
}

static int gc_classify_record(struct fffs *fs, size_t sector,
        const struct fffs_md_record *record, bool *live) {
    if (record->lifecycle != FFFS_MD_RECORD_LIVE) {
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
                record->slot, FFFS_TOMBSTONE_NO_ACCOUNTING, NULL);
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
        fs->gc_md.live_seen = false;
        fs->gc_md.active = false;
    }
    if (use_map && fffs_alloc_map_maybe_used(fs, (uint16_t)s)) {
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_md.live_seen = false;
        fs->gc_md.active = false;
        if (out_action) {
            *out_action = FFFS_GC_SCANNED;
        }
        return FFFS_OK;
    }
    enum gc_sector_state state;
    bool full_hint;
    int err = sector_footer_state(fs, s, &state, &full_hint);
    if (err != FFFS_OK) {
        return err;
    }
    if (state == GC_SECTOR_BLANK) {
        fffs_alloc_map_mark_unknown(fs, (uint16_t)s);
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_md.live_seen = false;
        if (out_action) {
            *out_action = FFFS_GC_SCANNED;
        }
        return FFFS_OK;
    }
    if (fffs_sector_is_inflight(fs, (uint16_t)s)) {
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_md.live_seen = true;
        fs->gc_md.active = false;
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
        fs->gc_md.live_seen = false;
        fs->gc_md.active = false;
        if (out_action) {
            *out_action = FFFS_GC_ERASED;
        }
        return FFFS_OK;
    }

    uint8_t window_buf[FFFS_MD_PRELOAD_MAX];
    struct fffs_md_read_window window = {
        .data = window_buf,
        .capacity = sizeof(window_buf),
        .sector = (uint16_t)s,
    };
    if (!fs->gc_md.active || fs->gc_md.sector != s) {
        err = fffs_md_walk_init(fs, &fs->gc_md, (uint16_t)s, &window);
        if (err != FFFS_OK) {
            return err;
        }
    }

    struct fffs_md_record record;
    enum fffs_md_walk_result walk_result;
    err = fffs_md_walk_next(fs, &fs->gc_md, &window, &record, &walk_result);
    if (err == FFFS_ERR_CORRUPT) {
        fs->gc_md.active = false;
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
        fs->gc_md.active = false;
        return err;
    }
    if (walk_result == FFFS_MD_WALK_END_INVALID) {
        uint8_t tombstone = FFFS_MD_FLAGS_TOMBSTONED;
        err = fffs_flash_program_aligned(fs,
                s * fs->sector_size + record.record_start,
                &tombstone, sizeof(tombstone));
        fs->gc_md.active = false;
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        if (err != FFFS_OK) {
            return err;
        }
        if (out_action) {
            *out_action = FFFS_GC_TOMBSTONED;
        }
        return FFFS_OK;
    }
    if (walk_result == FFFS_MD_WALK_RECORD) {
        bool record_live = false;
        err = gc_classify_record(fs, s, &record, &record_live);
        if (err != FFFS_OK) {
            fs->gc_md.active = false;
            return err;
        }
        if (record_live) {
            fs->gc_md.live_seen = true;
        }
    }

    if (walk_result == FFFS_MD_WALK_RECORD && fs->gc_md.active) {
        if (out_action) {
            *out_action = record.lifecycle == FFFS_MD_RECORD_LIVE &&
                !fs->gc_md.live_seen ? FFFS_GC_TOMBSTONED : FFFS_GC_SCANNED;
        }
        return FFFS_OK;
    }

    if (!fs->gc_md.live_seen) {
        bool reachable_tail = false;
        err = sector_is_reachable_from_any_chain(fs, s, &reachable_tail);
        if (err != FFFS_OK) {
            return err;
        }
        fs->gc_md.live_seen = reachable_tail;
    }

    if (fs->gc_md.live_seen) {
        if (full_hint) {
            fffs_alloc_map_mark_used(fs, (uint16_t)s);
        }
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_md.active = false;
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
    fs->gc_md.active = false;
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
