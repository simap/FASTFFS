/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS garbage collection control: incremental sector reclaim steps,
 * bounded allocation-pressure reclaim, and public GC convenience loops.
 */

#include "fffs_internal.h"

#include <string.h>


static size_t normalized_data_cursor(struct fffs *fs, size_t sector) {
    if (sector < fs->index_sectors || sector >= fs->sector_count) {
        return fs->index_sectors;
    }
    return sector;
}

/*
 * GC-step liveness helper. This is split out only so gc_classify_record() and
 * the paranoid GC check can walk a current file chain without burying the
 * sector scan logic.
 */
static int sector_is_reachable_from_chain(struct fffs *fs, uint16_t slot,
        uint16_t head, size_t sector, bool *reachable) {
    uint16_t current = head;
    FFFS_PROFILE_PUSH(fs, FFFS_PROFILE_GC_REACHABILITY);
    for (size_t depth = 0; current != 0 && depth < fs->sector_count; depth++) {
        struct fffs_md_record record;
        int err = fffs_read_md_for_slot(fs, current, slot, &record);
        if (err != FFFS_OK) {
            FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC_REACHABILITY);
            return err;
        }
        size_t span_end = (size_t)current + record.span_len;
        if (span_end > fs->sector_count) {
            FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC_REACHABILITY);
            return FFFS_ERR_CORRUPT;
        }
        if (sector >= current && sector < span_end) {
            *reachable = true;
            FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC_REACHABILITY);
            return FFFS_OK;
        }
        current = record.next;
    }
    if (current != 0) {
        FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC_REACHABILITY);
        return FFFS_ERR_CORRUPT;
    }
    FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC_REACHABILITY);
    return FFFS_OK;
}

static bool compaction_candidate_better(
        const struct fffs_compaction_candidate *candidate,
        const struct fffs_compaction_candidate *other) {
    if (candidate->sector == 0) {
        return false;
    }
    if (other->sector == 0) {
        return true;
    }
    if (candidate->trapped_reclaimable != other->trapped_reclaimable) {
        return candidate->trapped_reclaimable >
            other->trapped_reclaimable;
    }
    if (candidate->live_root_count != other->live_root_count) {
        return candidate->live_root_count > other->live_root_count;
    }
    return false;
}

static void gc_update_compaction_candidate(struct fffs *fs, uint16_t sector,
        size_t immediate_free) {
    size_t accounted = fs->gc_md.live_bytes + immediate_free;
    size_t trapped = accounted < fs->sector_size ?
        fs->sector_size - accounted : 0;
    if (trapped > UINT16_MAX) {
        trapped = UINT16_MAX;
    }

    struct fffs_compaction_candidate candidate = {
        .sector = sector,
        .trapped_reclaimable = (uint16_t)trapped,
        .live_root_count = fs->gc_md.live_root_count,
    };

    size_t pos = FFFS_COMPACTION_CANDIDATE_COUNT - 1u;
    if (!compaction_candidate_better(&candidate,
                &fs->compaction_candidates[pos])) {
        return;
    }

    fs->compaction_candidates[pos] = candidate;
    while (pos > 0 &&
            compaction_candidate_better(&fs->compaction_candidates[pos],
                &fs->compaction_candidates[pos - 1u])) {
        struct fffs_compaction_candidate tmp =
            fs->compaction_candidates[pos - 1u];
        fs->compaction_candidates[pos - 1u] = fs->compaction_candidates[pos];
        fs->compaction_candidates[pos] = tmp;
        pos--;
    }
}

static int gc_fsinfo_allows_compaction(struct fffs *fs,
        const struct fffs_compaction_candidate *candidate, bool *allowed) {
    *allowed = false;

    struct fffs_fsinfo info;
    int err = fffs_fsinfo(fs, &info, FFFS_FSINFO_REFRESH_IF_NEEDED |
            FFFS_FSINFO_ESTIMATE_METADATA);
    if (err != FFFS_OK) {
        return err;
    }

    uint32_t required = fs->sector_size > candidate->trapped_reclaimable ?
        (uint32_t)(fs->sector_size - candidate->trapped_reclaimable) :
        FFFS_ALLOC_MIN_USABLE_FREE;
    uint32_t needed_flags = FFFS_FSINFO_TOTAL_VALID |
        FFFS_FSINFO_COMMITTED_BYTES_VALID | FFFS_FSINFO_PENDING_VALID |
        FFFS_FSINFO_METADATA_ESTIMATE_VALID;
    if ((info.valid_flags & needed_flags) != needed_flags) {
        return FFFS_OK;
    }

    uint32_t used = info.committed_data_bytes + info.pending_data_bytes +
        info.estimated_metadata_bytes;
    if (info.total_bytes <= used) {
        return FFFS_OK;
    }
    *allowed = info.total_bytes - used >= required;
    return FFFS_OK;
}

static int gc_copy_root_data(struct fffs *fs, uint16_t source_sector,
        const struct fffs_md_record *record, uint16_t dest_sector,
        uint16_t dest_data_off) {
    size_t src = (size_t)source_sector * fs->sector_size + record->data_off;
    size_t dst = (size_t)dest_sector * fs->sector_size + dest_data_off;
    size_t remaining = record->data_len;
    while (remaining != 0) {
        size_t n = remaining < fs->scratch_size ? remaining :
            fs->scratch_size;
        int err = fffs_flash_read(fs, src, fs->scratch, n);
        if (err != FFFS_OK) {
            return err;
        }
        fffs_scratch_bump(fs);
        err = fffs_flash_program_aligned(fs, dst, fs->scratch, n);
        if (err != FFFS_OK) {
            return err;
        }
        src += n;
        dst += n;
        remaining -= n;
    }
    return FFFS_OK;
}

static int gc_copy_root_record(struct fffs *fs, uint16_t source_sector,
        const struct fffs_md_record *record, bool allow_relaxed_compaction) {
    uint16_t dest_sector;
    uint16_t data_off;
    uint16_t record_off;
    bool needs_footer;
    int err = fffs_alloc_find_compaction_root_window(fs, source_sector,
            record->slot, record->data_len, &dest_sector, &data_off,
            &record_off, &needs_footer, allow_relaxed_compaction);
    if (err != FFFS_OK) {
        return err;
    }

    err = gc_copy_root_data(fs, source_sector, record, dest_sector, data_off);
    if (err != FFFS_OK) {
        return err;
    }

    uint16_t next = record->span_len > 1 ?
        (uint16_t)(source_sector + 1u) : record->next;
    err = fffs_program_extent_metadata(fs, dest_sector, record->slot,
            FFFS_MD_TYPE_FILE_ROOT_V1, data_off, record_off, needs_footer,
            record->data_len, 1, record->size_or_offset, next, true);
    if (err != FFFS_OK) {
        return err;
    }
    return fffs_append_index_record(fs, record->slot, dest_sector);
}

static int gc_tombstone_copied_roots(struct fffs *fs, uint16_t sector,
        size_t copied_through_entry) {
    uint8_t window_buf[FFFS_MD_PRELOAD_MAX];
    struct fffs_sector_reader window = {
        .data = window_buf,
        .capacity = sizeof(window_buf),
        .sector = sector,
        .reverse = true,
    };
    struct fffs_md_walk walk;
    int err = fffs_md_walk_init(fs, &walk, sector, &window);
    if (err != FFFS_OK) {
        return err;
    }

    size_t entry = 0;
    while (walk.active && entry < copied_through_entry) {
        struct fffs_md_record record;
        enum fffs_md_walk_result result;
        err = fffs_md_walk_next(fs, &walk, &window, &record, &result);
        if (err != FFFS_OK) {
            return err;
        }
        if (result != FFFS_MD_WALK_RECORD) {
            break;
        }
        if (record.live && record.type == FFFS_MD_TYPE_FILE_ROOT_V1) {
            bool accounted = false;
            err = fffs_tombstone_metadata_for_slot(fs, sector, record.slot,
                    FFFS_TOMBSTONE_NO_ACCOUNTING, &accounted);
            if (err != FFFS_OK) {
                return err;
            }
            fffs_alloc_map_mark_unknown(fs, sector);
        }
        entry++;
    }
    return FFFS_OK;
}

static int gc_compact_root_only_sector(struct fffs *fs, uint16_t source_sector,
        uint16_t *erased_sector, bool allow_relaxed_compaction) {
    uint8_t window_buf[FFFS_MD_PRELOAD_MAX];
    struct fffs_sector_reader window = {
        .data = window_buf,
        .capacity = sizeof(window_buf),
        .sector = source_sector,
        .reverse = true,
    };
    struct fffs_md_walk walk;
    int err = fffs_md_walk_init(fs, &walk, source_sector, &window);
    if (err != FFFS_OK) {
        return err;
    }

    size_t entry = 0;
    size_t copied_through_entry = 0;
    while (walk.active) {
        struct fffs_md_record record;
        enum fffs_md_walk_result result;
        err = fffs_md_walk_next(fs, &walk, &window, &record, &result);
        if (err != FFFS_OK) {
            goto fail;
        }
        if (result == FFFS_MD_WALK_END_INVALID) {
            err = FFFS_ERR_NO_SPACE;
            goto fail;
        }
        if (result != FFFS_MD_WALK_RECORD) {
            break;
        }

        if (record.live) {
            if (record.type != FFFS_MD_TYPE_FILE_ROOT_V1 ||
                    fffs_slot_is_pinned(fs, record.slot)) {
                err = FFFS_ERR_NO_SPACE;
                goto fail;
            }
            err = gc_copy_root_record(fs, source_sector, &record,
                    allow_relaxed_compaction);
            if (err != FFFS_OK) {
                goto fail;
            }
            copied_through_entry = entry + 1u;
        }
        entry++;
    }

    err = fffs_map_backend_status(fs->backend.erase(fs->backend.ctx,
                (size_t)source_sector * fs->sector_size, fs->sector_size));
#if FFFS_PROFILE_TRACE
    fffs_profile_flash(fs, FFFS_PROFILE_FLASH_ERASE,
            (size_t)source_sector * fs->sector_size, fs->sector_size);
#endif
    if (err != FFFS_OK) {
        goto fail;
    }
    fffs_alloc_map_mark_unknown(fs, source_sector);
    *erased_sector = source_sector;
    return FFFS_OK;

fail:
    if (copied_through_entry != 0) {
        int cleanup_err = gc_tombstone_copied_roots(fs, source_sector,
                copied_through_entry);
        if (cleanup_err != FFFS_OK) {
            return cleanup_err;
        }
    }
    return err;
}

int fffs_gc_compact_until_erased(struct fffs *fs, uint16_t *erased_sector,
        bool allow_relaxed_compaction) {
    for (size_t i = 0; i < FFFS_COMPACTION_CANDIDATE_COUNT; i++) {
        const struct fffs_compaction_candidate *candidate =
            &fs->compaction_candidates[i];
        if (candidate->sector == 0) {
            break;
        }

        bool allowed;
        int err = gc_fsinfo_allows_compaction(fs, candidate, &allowed);
        if (err != FFFS_OK) {
            return err;
        }
        if (!allowed) {
            continue;
        }
        err = gc_compact_root_only_sector(fs, candidate->sector,
                erased_sector, allow_relaxed_compaction);
        if (err == FFFS_OK) {
            return FFFS_OK;
        }
        if (err != FFFS_ERR_NO_SPACE) {
            return err;
        }
    }
    return FFFS_ERR_NO_SPACE;
}

#if FFFS_GC_PARANOID_REACHABILITY
static int gc_paranoid_sector_is_reachable_from_any_chain(struct fffs *fs,
        size_t sector, bool *reachable) {
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
#endif

/*
 * Core gc_step liveness classifier. This is not a general-purpose helper:
 * gc_step() uses it while scanning sector metadata to decide whether a
 * live-looking record is still reachable from the current index chain, and to
 * tombstone records that are not.
 */
static int gc_classify_record(struct fffs *fs, size_t sector,
        const struct fffs_md_record *record, bool *live) {
    if (!record->live) {
        return FFFS_OK;
    }
    if (fffs_slot_is_pinned(fs, record->slot)) {
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
        bool use_map, bool *out_cheap_skip) {
    if (!fs) {
        return FFFS_ERR_INVALID;
    }
    if (out_action) {
        *out_action = FFFS_GC_IDLE;
    }
    /*
     * "Cheap skip" means this call classified the sector from in-memory state
     * alone (no flash access): the alloc map, the compaction reservation, or a
     * writer holding it in flight.
     */
    if (out_cheap_skip) {
        *out_cheap_skip = true;
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
    if (fffs_alloc_compaction_reserved(fs, (uint16_t)s)) {
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_md.live_seen = false;
        fs->gc_md.active = false;
        if (out_action) {
            *out_action = FFFS_GC_SCANNED;
        }
        return FFFS_OK;
    }
    if (fffs_sector_is_open_for_writing(fs, (uint16_t)s)) {
        fs->gc_cursor = fffs_next_data_sector(fs, s);
        fs->gc_md.live_seen = true;
        fs->gc_md.active = false;
        if (out_action) {
            *out_action = FFFS_GC_SCANNED;
        }
        return FFFS_OK;
    }

    if (out_cheap_skip) {
        *out_cheap_skip = false; //past the cheap in-memory cases.
    }
    uint8_t window_buf[FFFS_MD_PRELOAD_MAX];
    struct fffs_sector_reader window = {
        .data = window_buf,
        .capacity = sizeof(window_buf),
        .sector = (uint16_t)s,
        .reverse = true,
    };
    const uint8_t *footer_bytes;
    FFFS_PROFILE_PUSH(fs, FFFS_PROFILE_GC_FOOTER_STATE);
    int err = fffs_sector_reader_view(fs, &window, (uint16_t)s,
            fs->sector_size - FFFS_SECTOR_FOOTER_SIZE,
            FFFS_SECTOR_FOOTER_SIZE, &footer_bytes);
    FFFS_PROFILE_POP(fs, FFFS_PROFILE_GC_FOOTER_STATE);
    if (err != FFFS_OK) {
        return err;
    }

    struct fffs_sector_footer footer;
    fffs_decode_sector_footer(footer_bytes, &footer);
    bool erase_sector = false;
    if (footer.erased) {
        err = fffs_flash_span_is_erased(fs, s * fs->sector_size,
                fs->sector_size);
        if (err == FFFS_OK) {
            fffs_alloc_map_mark_unknown(fs, (uint16_t)s);
            fs->gc_cursor = fffs_next_data_sector(fs, s);
            fs->gc_md.live_seen = false;
            fs->gc_md.active = false;
            if (out_action) {
                *out_action = FFFS_GC_ERASED;
            }
            return FFFS_OK;
        }
        if (err != FFFS_ERR_NO_SPACE) {
            return err;
        }
        erase_sector = true;
    } else if (footer.type != FFFS_SECTOR_TYPE_FILE || !footer.magic_valid ||
            footer.valid_bits == FFFS_BITMIRROR_MIXED ||
            footer.tombstone_bits == FFFS_BITMIRROR_MIXED ||
            footer.state == 0xff ||
            footer.tombstone_bits == FFFS_BITMIRROR_CLEARED) {
        erase_sector = true;
    } else if (!fffs_lifecycle_is_live(footer.valid_bits,
                footer.tombstone_bits)) {
        return FFFS_ERR_CORRUPT;
    }

    if (erase_sector) {
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

    enum fffs_md_walk_result walk_result;
    if (!fs->gc_md.active || fs->gc_md.sector != s) {
        err = fffs_md_walk_init(fs, &fs->gc_md, (uint16_t)s, &window);
        if (err != FFFS_OK) {
            return err;
        }
    }

    struct fffs_md_record record;
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
        fs->gc_md.active = false;
#if FFFS_GC_PARANOID_REACHABILITY
        bool reachable_tail = false;
        err = gc_paranoid_sector_is_reachable_from_any_chain(fs, s,
                &reachable_tail);
        if (err != FFFS_OK || reachable_tail) {
            return err == FFFS_OK ? FFFS_ERR_CORRUPT : err;
        }
#endif
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
            fs->gc_md.live_bytes +=
                (uint32_t)record.data_len + FFFS_MD_FILE_RECORD_SIZE;
            if (record.type == FFFS_MD_TYPE_FILE_ROOT_V1) {
                fs->gc_md.live_root_seen = true;
                fs->gc_md.live_root_count++;
            } else if (record.type == FFFS_MD_TYPE_FILE_CONT_V1) {
                fs->gc_md.live_continuation_seen = true;
            }
            if (fffs_slot_is_pinned(fs, record.slot)) {
                fs->gc_md.open_seen = true;
            }
        }
    }

    if (walk_result == FFFS_MD_WALK_RECORD && fs->gc_md.active) {
        if (out_action) {
            *out_action = record.live && !fs->gc_md.live_seen ?
                FFFS_GC_TOMBSTONED : FFFS_GC_SCANNED;
        }
        return FFFS_OK;
    }

    if (!fs->gc_md.live_seen) {
#if FFFS_GC_PARANOID_REACHABILITY
        bool reachable_tail = false;
        err = gc_paranoid_sector_is_reachable_from_any_chain(fs, s,
                &reachable_tail);
        if (err != FFFS_OK) {
            return err;
        }
        fs->gc_md.live_seen = reachable_tail;
#endif
    }

    if (fs->gc_md.live_seen) {
        bool root_only_compaction_candidate = fs->gc_md.live_root_seen &&
            !fs->gc_md.live_continuation_seen && !fs->gc_md.open_seen;
        bool normal_alloc_full = fs->gc_md.live_continuation_seen ||
            fs->gc_md.live_root_count >=
                FFFS_ALLOC_MAX_MD_RECORDS_PER_SECTOR;
        size_t immediate_free = 0;
        if ((root_only_compaction_candidate || !normal_alloc_full) &&
                walk_result == FFFS_MD_WALK_END_ERASED &&
                fs->gc_md.cursor > fs->gc_md.claimed_data_end) {
            size_t free_len = fs->gc_md.cursor -
                fs->gc_md.claimed_data_end;
            if (free_len >= FFFS_ALLOC_MIN_USABLE_FREE) {
                //FIXME: this check is expensive for candidate selection, but catches sectors where a crash has left junk filling up the sector without a MD entry.
                err = fffs_flash_span_is_erased(fs,
                        (size_t)s * fs->sector_size +
                            fs->gc_md.claimed_data_end,
                        free_len);
                if (err == FFFS_OK) {
                    immediate_free = free_len;
                } else if (err != FFFS_ERR_NO_SPACE) {
                    return err;
                }
            }
        }
        if (root_only_compaction_candidate) {
            gc_update_compaction_candidate(fs, (uint16_t)s,
                    immediate_free);
        }
        if (!normal_alloc_full) {
            size_t required_window = FFFS_ALLOC_MIN_USABLE_FREE +
                FFFS_MD_FILE_RECORD_SIZE;
            normal_alloc_full = immediate_free < required_window;
        }
        if (normal_alloc_full) {
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
    if (!fs) {
        return FFFS_ERR_INVALID;
    }
    /*
     * Keep scanning past sectors that gc_step() can classify cheaply so a
     * single public step lands on real GC work instead of being spent on a 
     * cheap skip. We stop after one full pass around the ring rather than 
     * spinning.
     */
    size_t data_sectors = fs->sector_count > fs->index_sectors ?
        fs->sector_count - fs->index_sectors : 0;
    for (size_t skipped = 0;; skipped++) {
        bool cheap_skip = false;
        int err = gc_step(fs, out_action, true, &cheap_skip);
        if (err != FFFS_OK || !cheap_skip || skipped + 1 >= data_sectors) {
            return err;
        }
    }
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
        memset(fs->compaction_candidates, 0,
                sizeof(fs->compaction_candidates));
        bool use_map = pass == 0;
        size_t advanced = 0;
        while (advanced < data_sectors) {
            size_t before = normalized_data_cursor(fs, fs->gc_cursor);
            enum fffs_gc_action action;
            int err = gc_step(fs, &action, use_map, NULL);
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
    return fffs_gc_compact_until_erased(fs, erased_sector, false);
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
