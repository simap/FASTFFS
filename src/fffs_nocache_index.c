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
    *slot = fffs_load_le16(rec);
    *head = fffs_load_le16(rec + 2);
    *erased = *slot == UINT16_MAX && *head == UINT16_MAX;
    if (*erased) {
        return FFFS_OK;
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
            if (rec_slot == 0 && head == 0) {
                continue;
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

static int resolve_reached_slot(struct fffs *fs, uint16_t base,
        uint32_t occupied[FFFS_PROBE_BITSET_WORDS], uint16_t *slot) {
    for (size_t d = 0; d <= FFFS_MAX_PROBE_DISTANCE; d++) {
        uint16_t candidate = candidate_slot(base, d);
        if (!fffs_bitset_get(occupied, d) &&
                !fffs_slot_is_pinned(fs, candidate)) {
            *slot = candidate;
            return FFFS_OK;
        }
    }
    return FFFS_ERR_NO_SPACE;
}

bool fffs_index_cache_config_valid(size_t count) {
    (void)count;
    return true;
}

size_t fffs_index_cache_required_size(size_t count) {
    (void)count;
    return 0;
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
        struct fffs_stat *out_st, uint16_t *payload_data_off,
        uint16_t *payload_data_len, uint16_t *next,
        struct fffs_read_cache_view *cache) {
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
            if (rec_slot == 0 && rec_head == 0) {
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
            uint16_t md_payload_data_off;
            uint16_t md_payload_data_len;
            uint16_t md_next;
            err = fffs_read_file_root_md(fs, rec_head, rec_slot, &st,
                    &md_payload_data_off, &md_payload_data_len, &md_next,
                    NULL, NULL, cache);
            if (err != FFFS_OK) {
                return err;
            }
            if (strcmp(st.name, name) == 0) {
                *slot = rec_slot;
                *head = rec_head;
                *found = true;
                if (out_st) {
                    *out_st = st;
                }
                if (payload_data_off) {
                    *payload_data_off = md_payload_data_off;
                }
                if (payload_data_len) {
                    *payload_data_len = md_payload_data_len;
                }
                if (next) {
                    *next = md_next;
                }
                return FFFS_OK;
            }
        }
    }

    int err = resolve_reached_slot(fs, base, occupied, slot);
    if (err != FFFS_OK) {
        return err;
    }
    *head = 0;
    *found = false;
    return FFFS_OK;
}

static bool current_index_entry_read(struct fffs *fs, size_t *cursor_seq_pos,
        size_t *cursor_offset, struct fffs_dir *dir, uint16_t *slot,
        uint16_t *head, int *status) {
    struct index_scan scan;
    index_scan_init(&scan, fs, dir, fs->scratch, fs->scratch_size, true);
    struct logical_pos cur = {
        .seq_pos = *cursor_seq_pos,
        .offset = *cursor_offset,
    };
    if (cur.seq_pos == 0 && cur.offset == 0) {
        cur.offset = logical_sector_begin(fs, 0);
    }

    for (; cur.seq_pos < fs->index_sequence_count; cur.seq_pos++) {
        size_t end = logical_sector_end(fs, cur.seq_pos);
        for (; cur.offset + 4 <= end; cur.offset += 4) {
            struct logical_pos rec_pos = cur;
            *cursor_seq_pos = cur.seq_pos;
            *cursor_offset = cur.offset + 4;

            uint16_t rec_slot;
            uint16_t rec_head;
            bool erased;
            int err = read_index_at(&scan, cur.offset, &rec_slot, &rec_head,
                    &erased);
            if (err != FFFS_OK) {
                *status = err;
                return false;
            }
            if (erased) {
                break;
            }
            if (rec_slot == 0 && rec_head == 0) {
                continue;
            }
            if (rec_head == 0) {
                continue;
            }

            bool newer;
            err = newer_record_for_slot(&scan, &rec_pos, rec_slot, &newer);
            if (err != FFFS_OK) {
                *status = err;
                return false;
            }
            if (newer) {
                continue;
            }

            *slot = rec_slot;
            *head = rec_head;
            *status = FFFS_OK;
            return true;
        }
        if (cur.seq_pos + 1 < fs->index_sequence_count) {
            cur.offset = logical_sector_begin(fs, cur.seq_pos + 1);
            *cursor_seq_pos = cur.seq_pos + 1;
            *cursor_offset = cur.offset;
        }
    }
    *status = FFFS_OK;
    return false;
}

bool fffs_index_dir_read(struct fffs_dir *dir, struct fffs_stat *st) {
    struct fffs *fs = dir->fs;
    while (true) {
        uint16_t slot;
        uint16_t head;
        if (!current_index_entry_read(fs, &dir->cursor_seq_pos,
                    &dir->cursor_offset, dir, &slot, &head, &dir->status)) {
            return false;
        }

        struct fffs_stat candidate;
        int err = fffs_read_file_root_md(fs, head, slot, &candidate,
                NULL, NULL, NULL, NULL, NULL, NULL);
        if (err != FFFS_OK) {
            dir->status = err;
            return false;
        }
        if (dir->prefix_len &&
                strncmp(candidate.name, dir->prefix, dir->prefix_len) != 0) {
            continue;
        }
        *st = candidate;
        dir->status = FFFS_OK;
        return true;
    }
    dir->status = FFFS_OK;
    return false;
}

bool fffs_index_iter_read(struct fffs_index_iter *iter, uint16_t *slot,
        uint16_t *head) {
    return current_index_entry_read(iter->fs, &iter->cursor_seq_pos,
            &iter->cursor_offset, NULL, slot, head, &iter->status);
}

int fffs_index_head_for_slot(struct fffs *fs, uint16_t candidate_slot,
        uint16_t *head, bool *found) {
    struct index_scan scan;
    index_scan_init(&scan, fs, NULL, fs->scratch, fs->scratch_size, true);
    *head = 0;
    *found = false;
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
            int err = read_index_at(&scan, off, &slot, &rec_head, &erased);
            if (err != FFFS_OK) {
                return err;
            }
            if (erased || slot != candidate_slot) {
                continue;
            }
            *head = rec_head;
            *found = rec_head != 0;
            return FFFS_OK;
        }
    }
    return FFFS_OK;
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

#endif
