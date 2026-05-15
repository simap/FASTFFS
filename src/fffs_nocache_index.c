/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS no-cache namespace index backend: performs on-demand logical index
 * scans without keeping a persistent RAM map of file slots to head sectors.
 */

#include "fffs_internal.h"

#include <string.h>

#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_NONE

enum {
    FFFS_PROBE_BITS = FFFS_MAX_PROBE_DISTANCE + 1u,
    FFFS_BITSET_WORD_BITS = 32u,
    FFFS_PROBE_BITSET_WORDS =
        (FFFS_PROBE_BITS + FFFS_BITSET_WORD_BITS - 1u) /
        FFFS_BITSET_WORD_BITS,
};

struct logical_pos {
    size_t seq_pos;
    size_t offset;
};

struct index_scan {
    struct fffs *fs;
    struct fffs_dir *dir;
    uint8_t *window;
    size_t window_size;
    size_t window_start;
    size_t window_len;
    bool uses_fs_scratch;
};

static uint16_t load16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static size_t logical_sector(const struct fffs *fs, size_t seq_pos) {
    return (fs->oldest_index_sector + seq_pos) % fs->index_sectors;
}

static size_t logical_sector_begin(const struct fffs *fs, size_t seq_pos) {
    return logical_sector(fs, seq_pos) * fs->sector_size + FFFS_HEADER_SIZE;
}

static size_t logical_sector_end(const struct fffs *fs, size_t seq_pos) {
    size_t sector = logical_sector(fs, seq_pos);
    if (sector == fs->active_index_sector) {
        return fs->next_index_offset;
    }
    return (sector + 1) * fs->sector_size;
}

static bool candidate_offset(uint16_t base, uint16_t slot, size_t *offset) {
    uint16_t candidate = base;
    for (size_t d = 0; d <= FFFS_MAX_PROBE_DISTANCE; d++) {
        if (candidate == slot) {
            *offset = d;
            return true;
        }
        candidate = fffs_next_slot(candidate);
    }
    return false;
}

static uint16_t candidate_slot(uint16_t base, size_t offset) {
    uint16_t slot = base;
    for (size_t d = 0; d < offset; d++) {
        slot = fffs_next_slot(slot);
    }
    return slot;
}

static void index_scan_init(struct index_scan *scan, struct fffs *fs,
        struct fffs_dir *dir, uint8_t *window, size_t window_size,
        bool uses_fs_scratch) {
    *scan = (struct index_scan){0};
    scan->fs = fs;
    scan->dir = dir;
    scan->window = window;
    scan->window_size = window_size - (window_size % 4);
    scan->uses_fs_scratch = uses_fs_scratch;

    if (dir && scan->uses_fs_scratch &&
            dir->scratch_serial == fs->scratch_serial &&
            dir->cached_len != 0) {
        scan->window_start = dir->cached_offset;
        scan->window_len = dir->cached_len;
    }
}

static int index_scan_load(struct index_scan *scan, size_t offset) {
    if (offset >= scan->window_start &&
            offset + 4 <= scan->window_start + scan->window_len) {
        return FFFS_OK;
    }

    struct fffs *fs = scan->fs;
    size_t sector = offset / fs->sector_size;
    size_t begin = sector * fs->sector_size + FFFS_HEADER_SIZE;
    size_t end = (sector + 1) * fs->sector_size;
    size_t rel = offset - begin;
    size_t base = begin + rel - (rel % scan->window_size);
    size_t nread = end - base;
    if (nread > scan->window_size) {
        nread = scan->window_size;
    }
    nread -= nread % 4;
    if (nread < 4) {
        return FFFS_ERR_CORRUPT;
    }

    int err = fffs_flash_read(fs, base, scan->window, nread);
    if (err != FFFS_OK) {
        return err;
    }
    scan->window_start = base;
    scan->window_len = nread;
    if (scan->uses_fs_scratch) {
        fffs_scratch_bump(fs);
        if (scan->dir) {
            scan->dir->scratch_serial = fs->scratch_serial;
            scan->dir->cached_seq_pos = sector;
            scan->dir->cached_offset = base;
            scan->dir->cached_len = nread;
        }
    }
    return FFFS_OK;
}

static int read_index_at(struct index_scan *scan, size_t offset,
        uint16_t *slot, uint16_t *head, bool *erased) {
    int err = index_scan_load(scan, offset);
    if (err != FFFS_OK) {
        return err;
    }
    const uint8_t *rec = scan->window + (offset - scan->window_start);
    *slot = load16(rec);
    *head = load16(rec + 2);
    *erased = *slot == UINT16_MAX && *head == UINT16_MAX;
    if (*erased) {
        return FFFS_OK;
    }
    if (*slot == 0 && *head == 0) {
        return FFFS_ERR_CORRUPT;
    }
    return FFFS_OK;
}

static int newer_record_for_slot(struct index_scan *scan,
        const struct logical_pos *pos, uint16_t slot, bool *newer) {
    struct fffs *fs = scan->fs;
    *newer = false;
    struct logical_pos cur = {
        .seq_pos = pos->seq_pos,
        .offset = pos->offset + 4,
    };
    for (; cur.seq_pos < fs->index_sequence_count; cur.seq_pos++) {
        size_t end = logical_sector_end(fs, cur.seq_pos);
        for (; cur.offset + 4 <= end; cur.offset += 4) {
            uint16_t rec_slot;
            uint16_t head;
            bool erased;
            int err = read_index_at(scan, cur.offset, &rec_slot, &head,
                    &erased);
            if (err != FFFS_OK) {
                return err;
            }
            if (erased) {
                break;
            }
            if (rec_slot == slot) {
                *newer = true;
                return FFFS_OK;
            }
        }
        if (cur.seq_pos + 1 < fs->index_sequence_count) {
            cur.offset = logical_sector_begin(fs, cur.seq_pos + 1);
        }
    }
    return FFFS_OK;
}

static int resolve_reached_slot(uint16_t base,
        uint32_t occupied[FFFS_PROBE_BITSET_WORDS], uint16_t *slot) {
    for (size_t d = 0; d <= FFFS_MAX_PROBE_DISTANCE; d++) {
        if (!fffs_bitset_get(occupied, d)) {
            *slot = candidate_slot(base, d);
            return FFFS_OK;
        }
    }
    return FFFS_ERR_NO_SPACE;
}

bool fffs_index_cache_config_valid(size_t count) {
    (void)count;
    return true;
}

int fffs_index_candidate(struct fffs *fs, uint16_t slot, size_t probe,
        uint16_t *head, bool *end) {
    (void)fs;
    (void)slot;
    (void)probe;
    if (!head || !end) {
        return FFFS_ERR_INVALID;
    }
    *head = 0;
    *end = true;
    return FFFS_OK;
}

int fffs_index_insert(struct fffs *fs, uint16_t slot, uint16_t head) {
    (void)fs;
    (void)slot;
    (void)head;
    return FFFS_OK;
}

int fffs_index_remove(struct fffs *fs, uint16_t slot) {
    (void)fs;
    (void)slot;
    return FFFS_OK;
}

int fffs_index_set(struct fffs *fs, uint16_t slot, uint16_t head) {
    (void)fs;
    (void)slot;
    (void)head;
    return FFFS_OK;
}

int fffs_index_resolve(struct fffs *fs, const char *name,
        uint16_t *slot, uint16_t *head, bool *found,
        struct fffs_stat *out_st, uint16_t *data_off, uint16_t *data_len,
        uint16_t *next) {
    size_t name_len = strlen(name);
    if (name_len == 0) {
        return FFFS_ERR_INVALID;
    }
    if (name_len > FFFS_MAX_NAME) {
        return FFFS_ERR_NAME_TOO_LONG;
    }

    uint16_t base = fffs_normalize_slot_base(fffs_hash16(name));
    uint32_t occupied[FFFS_PROBE_BITSET_WORDS];
    uint32_t deleted[FFFS_PROBE_BITSET_WORDS];
    fffs_bitset_clear(occupied, FFFS_PROBE_BITSET_WORDS);
    fffs_bitset_clear(deleted, FFFS_PROBE_BITSET_WORDS);
    struct index_scan scan;
    index_scan_init(&scan, fs, NULL, fs->scratch, fs->scratch_size, true);

    for (size_t seq_count = fs->index_sequence_count; seq_count > 0;
            seq_count--) {
        size_t seq_pos = seq_count - 1;
        size_t begin = logical_sector_begin(fs, seq_pos);
        size_t off = logical_sector_end(fs, seq_pos);
        while (off >= begin + 4) {
            off -= 4;
            uint16_t rec_slot;
            uint16_t rec_head;
            bool erased;
            int err = read_index_at(&scan, off, &rec_slot, &rec_head,
                    &erased);
            if (err != FFFS_OK) {
                return err;
            }
            if (erased) {
                continue;
            }

            size_t d;
            if (!candidate_offset(base, rec_slot, &d) ||
                    fffs_bitset_get(occupied, d) ||
                    fffs_bitset_get(deleted, d)) {
                continue;
            }

            if (rec_head == 0) {
                fffs_bitset_set(deleted, d);
                continue;
            }
            if (rec_head < fs->index_sectors ||
                    rec_head >= fs->sector_count) {
                return FFFS_ERR_CORRUPT;
            }
            fffs_bitset_set(occupied, d);

            struct fffs_stat st;
            uint16_t md_slot;
            uint16_t md_data_off;
            uint16_t md_data_len;
            uint16_t md_next;
            err = fffs_read_metadata(fs, rec_head, &st, &md_slot,
                    &md_data_off, &md_data_len, &md_next);
            if (err == FFFS_ERR_CORRUPT) {
                continue;
            }
            if (err != FFFS_OK) {
                return err;
            }
            if (md_slot == rec_slot && strcmp(st.name, name) == 0) {
                *slot = rec_slot;
                *head = rec_head;
                *found = true;
                if (out_st) {
                    *out_st = st;
                }
                if (data_off) {
                    *data_off = md_data_off;
                }
                if (data_len) {
                    *data_len = md_data_len;
                }
                if (next) {
                    *next = md_next;
                }
                return FFFS_OK;
            }
        }
    }

    int err = resolve_reached_slot(base, occupied, slot);
    if (err != FFFS_OK) {
        return err;
    }
    *head = 0;
    *found = false;
    return FFFS_OK;
}

bool fffs_index_dir_read(struct fffs_dir *dir, struct fffs_stat *st) {
    struct fffs *fs = dir->fs;
    struct index_scan scan;
    index_scan_init(&scan, fs, dir, fs->scratch, fs->scratch_size, true);
    struct logical_pos cur = {
        .seq_pos = dir->cursor_seq_pos,
        .offset = dir->cursor_offset,
    };
    if (cur.seq_pos == 0 && cur.offset == 0) {
        cur.offset = logical_sector_begin(fs, 0);
    }

    for (; cur.seq_pos < fs->index_sequence_count; cur.seq_pos++) {
        size_t end = logical_sector_end(fs, cur.seq_pos);
        for (; cur.offset + 4 <= end; cur.offset += 4) {
            struct logical_pos rec_pos = cur;
            dir->cursor_seq_pos = cur.seq_pos;
            dir->cursor_offset = cur.offset + 4;

            uint16_t slot;
            uint16_t head;
            bool erased;
            int err = read_index_at(&scan, cur.offset, &slot, &head,
                    &erased);
            if (err != FFFS_OK) {
                dir->status = err;
                return false;
            }
            if (erased) {
                break;
            }
            if (head == 0) {
                continue;
            }

            bool newer;
            err = newer_record_for_slot(&scan, &rec_pos, slot, &newer);
            if (err != FFFS_OK) {
                dir->status = err;
                return false;
            }
            if (newer) {
                continue;
            }

            struct fffs_stat candidate;
            uint16_t md_slot;
            err = fffs_read_metadata(fs, head, &candidate, &md_slot,
                    NULL, NULL, NULL);
            if (err != FFFS_OK) {
                dir->status = err;
                return false;
            }
            if (md_slot != slot) {
                continue;
            }
            if (dir->prefix_len &&
                    strncmp(candidate.name, dir->prefix,
                        dir->prefix_len) != 0) {
                continue;
            }
            *st = candidate;
            dir->status = FFFS_OK;
            return true;
        }
        if (cur.seq_pos + 1 < fs->index_sequence_count) {
            cur.offset = logical_sector_begin(fs, cur.seq_pos + 1);
            dir->cursor_seq_pos = cur.seq_pos + 1;
            dir->cursor_offset = cur.offset;
        }
    }
    dir->status = FFFS_OK;
    return false;
}

int fffs_index_record_is_current(struct fffs *fs,
        size_t seq_pos, size_t offset, uint16_t slot, uint16_t head,
        bool *current) {
    (void)head;
    struct index_scan scan;
    struct logical_pos logical = {
        .seq_pos = seq_pos,
        .offset = offset,
    };
    bool newer;
    index_scan_init(&scan, fs, NULL, fs->scratch, fs->scratch_size, true);
    int err = newer_record_for_slot(&scan, &logical, slot, &newer);
    if (err != FFFS_OK) {
        return err;
    }
    *current = !newer;
    return FFFS_OK;
}

static int current_head_is_live(struct index_scan *scan, uint16_t head,
        bool *live) {
    struct fffs *fs = scan->fs;
    *live = false;
    for (size_t seq_count = fs->index_sequence_count; seq_count > 0;
            seq_count--) {
        size_t seq_pos = seq_count - 1;
        size_t begin = logical_sector_begin(fs, seq_pos);
        size_t off = logical_sector_end(fs, seq_pos);
        while (off >= begin + 4) {
            off -= 4;
            uint16_t slot;
            uint16_t rec_head;
            bool erased;
            int err = read_index_at(scan, off, &slot, &rec_head, &erased);
            if (err != FFFS_OK) {
                return err;
            }
            if (erased || rec_head == 0) {
                continue;
            }
            if (rec_head == head) {
                struct logical_pos pos = {
                    .seq_pos = seq_pos,
                    .offset = off,
                };
                bool newer;
                err = newer_record_for_slot(scan, &pos, slot, &newer);
                if (err != FFFS_OK) {
                    return err;
                }
                *live = !newer;
                return FFFS_OK;
            }
        }
    }
    return FFFS_OK;
}

bool fffs_index_sector_is_live_head(struct fffs *fs, size_t sector) {
    struct index_scan scan;
    index_scan_init(&scan, fs, NULL, fs->scratch, fs->scratch_size, true);
    bool live = false;
    int err = current_head_is_live(&scan, (uint16_t)sector, &live);
    return err == FFFS_OK && live;
}

void fffs_index_mark_live_heads_used(struct fffs *fs) {
    (void)fs;
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

int fffs_index_sector_is_live_extent(struct fffs *fs, size_t sector,
        bool *reachable) {
    struct index_scan scan;
    index_scan_init(&scan, fs, NULL, fs->scratch, fs->scratch_size, true);
    *reachable = false;
    for (size_t seq_count = fs->index_sequence_count; seq_count > 0;
            seq_count--) {
        size_t seq_pos = seq_count - 1;
        size_t begin = logical_sector_begin(fs, seq_pos);
        size_t off = logical_sector_end(fs, seq_pos);
        while (off >= begin + 4) {
            off -= 4;
            uint16_t slot;
            uint16_t head;
            bool erased;
            int err = read_index_at(&scan, off, &slot, &head, &erased);
            if (err != FFFS_OK) {
                return err;
            }
            if (erased || head == 0) {
                continue;
            }
            struct logical_pos pos = {
                .seq_pos = seq_pos,
                .offset = off,
            };
            bool newer;
            err = newer_record_for_slot(&scan, &pos, slot, &newer);
            if (err != FFFS_OK) {
                return err;
            }
            if (newer) {
                continue;
            }
            err = sector_is_reachable_from_chain(fs, head, sector,
                    reachable);
            if (err != FFFS_OK || *reachable) {
                return err;
            }
        }
    }
    return FFFS_OK;
}

#endif
