/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS in-RAM namespace index cache: direct slot and compact hash-head
 * cache implementations used by mount replay and namespace operations.
 */

#include "fffs_internal.h"

#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS
#define FFFS_INDEX_STALE_HEAD 1u

static bool is_power_of_two(size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

static int hash_remove_at(struct fffs *fs, size_t remove_idx);
static int hash_repair_from(struct fffs *fs, size_t hole);
#endif

bool fffs_index_hash_table_size_valid(size_t count) {
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
    return count >= FFFS_SLOT_COUNT;
#elif FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS
    return is_power_of_two(count) &&
        count <= FFFS_INDEX_HASH_TABLE_SIZE_MAX;
#else
#error "Unsupported FFFS_INDEX_CACHE_MODE"
#endif
}

int fffs_index_candidate(struct fffs *fs, uint16_t slot, size_t probe,
        uint16_t *head, bool *end) {
    if (!fs || !head || !end) {
        return FFFS_ERR_INVALID;
    }
    *head = 0;
    *end = false;
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
    if (probe > 0 || fs->index_heads[slot] == 0) {
        *end = true;
        return FFFS_OK;
    }
    *head = fs->index_heads[slot];
    return FFFS_OK;
#elif FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS
    if (probe >= fs->index_hash_table_size) {
        *end = true;
        return FFFS_OK;
    }
    size_t idx = (slot + probe) & (fs->index_hash_table_size - 1);
    uint16_t h = fs->index_heads[idx];
    if (h == 0 || h == FFFS_INDEX_STALE_HEAD) {
        *end = true;
        return FFFS_OK;
    }
    *head = h;
    return FFFS_OK;
#else
#error "Unsupported FFFS_INDEX_CACHE_MODE"
#endif
}

int fffs_index_insert(struct fffs *fs, uint16_t slot, uint16_t head) {
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
    fs->index_heads[slot] = head;
    return FFFS_OK;
#elif FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS
    size_t mask = fs->index_hash_table_size - 1;
    size_t idx = slot & mask;
restart:
    for (size_t probe = 0; probe < fs->index_hash_table_size; probe++) {
        uint16_t h = fs->index_heads[idx];
        if (h == 0) {
            fs->index_heads[idx] = head;
            return FFFS_OK;
        }
        uint16_t md_slot;
        int err = fffs_read_metadata(fs, h, NULL, &md_slot, NULL, NULL,
                NULL);
        if (err != FFFS_OK) {
            if (err != FFFS_ERR_CORRUPT) {
                return err;
            }
            err = hash_remove_at(fs, idx);
            if (err != FFFS_OK) {
                return err;
            }
            idx = slot & mask;
            goto restart;
        }
        if (md_slot == slot) {
            fs->index_heads[idx] = head;
            return FFFS_OK;
        }
        idx = (idx + 1) & mask;
    }
    return FFFS_ERR_NO_SPACE;
#else
#error "Unsupported FFFS_INDEX_CACHE_MODE"
#endif
}

#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS
static size_t probe_distance(size_t mask, size_t from, size_t to) {
    return (to - from) & mask;
}

static bool index_in_cluster_range(size_t mask, size_t start, size_t end,
        size_t idx) {
    return probe_distance(mask, start, idx) <=
        probe_distance(mask, start, end);
}

static bool cluster_start_for_bucket(struct fffs *fs, size_t idx,
        size_t *start, bool *full) {
    size_t mask = fs->index_hash_table_size - 1;
    if (fs->index_heads[idx] == 0) {
        return false;
    }
    *start = idx;
    *full = false;
    for (size_t scanned = 0; scanned + 1 < fs->index_hash_table_size;
            scanned++) {
        size_t prev = (*start - 1) & mask;
        if (fs->index_heads[prev] == 0) {
            return true;
        }
        *start = prev;
    }
    *full = true;
    return true;
}

static bool bucket_can_hold_slot(struct fffs *fs, size_t idx,
        uint16_t slot) {
    size_t start;
    bool full;
    if (!cluster_start_for_bucket(fs, idx, &start, &full)) {
        return false;
    }
    if (full) {
        return true;
    }
    size_t mask = fs->index_hash_table_size - 1;
    return index_in_cluster_range(mask, start, idx, slot & mask);
}

static void mark_stale_bucket(struct fffs *fs, size_t idx,
        size_t *first_stale, bool *have_stale) {
    fs->index_heads[idx] = FFFS_INDEX_STALE_HEAD;
    if (!*have_stale) {
        *first_stale = idx;
        *have_stale = true;
    }
}

static int hash_remove_at(struct fffs *fs, size_t remove_idx) {
    fs->index_heads[remove_idx] = FFFS_INDEX_STALE_HEAD;
    return hash_repair_from(fs, remove_idx);
}

static int hash_repair_from(struct fffs *fs, size_t hole) {
    size_t mask = fs->index_hash_table_size - 1;
    fs->index_heads[hole] = 0;
    size_t idx = (hole + 1) & mask;
    for (size_t scanned = 0; scanned < fs->index_hash_table_size &&
            fs->index_heads[idx] != 0; scanned++) {
        uint16_t h = fs->index_heads[idx];
        if (h == FFFS_INDEX_STALE_HEAD) {
            fs->index_heads[idx] = 0;
            idx = (idx + 1) & mask;
            continue;
        }
        uint16_t slot;
        int err = fffs_read_metadata(fs, h, NULL, &slot, NULL, NULL, NULL);
        if (err != FFFS_OK) {
            if (err != FFFS_ERR_CORRUPT) {
                return err;
            }
            fs->index_heads[idx] = 0;
            idx = (idx + 1) & mask;
            continue;
        }

        size_t home = slot & mask;
        if (probe_distance(mask, home, hole) <
                probe_distance(mask, home, idx)) {
            fs->index_heads[hole] = h;
            fs->index_heads[idx] = 0;
            hole = idx;
        }
        idx = (idx + 1) & mask;
    }
    return FFFS_OK;
}
#endif

int fffs_index_remove(struct fffs *fs, uint16_t slot) {
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
    fs->index_heads[slot] = 0;
    return FFFS_OK;
#elif FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS
    size_t mask = fs->index_hash_table_size - 1;
    size_t idx = slot & mask;
    size_t first_stale = 0;
    bool have_stale = false;
    for (size_t probe = 0; probe < fs->index_hash_table_size; probe++) {
        uint16_t h = fs->index_heads[idx];
        if (h == 0) {
            return have_stale ? hash_repair_from(fs, first_stale) : FFFS_OK;
        }
        if (h == FFFS_INDEX_STALE_HEAD) {
            idx = (idx + 1) & mask;
            continue;
        }
        uint16_t md_slot;
        int err = fffs_read_metadata(fs, h, NULL, &md_slot, NULL, NULL,
                NULL);
        if (err != FFFS_OK) {
            if (err != FFFS_ERR_CORRUPT) {
                return err;
            }
            mark_stale_bucket(fs, idx, &first_stale, &have_stale);
            idx = (idx + 1) & mask;
            continue;
        }
        if (md_slot == slot) {
            mark_stale_bucket(fs, idx, &first_stale, &have_stale);
            idx = (idx + 1) & mask;
            continue;
        }
        if (!bucket_can_hold_slot(fs, idx, md_slot)) {
            mark_stale_bucket(fs, idx, &first_stale, &have_stale);
        }
        idx = (idx + 1) & mask;
    }
    return have_stale ? hash_repair_from(fs, first_stale) : FFFS_OK;
#else
#error "Unsupported FFFS_INDEX_CACHE_MODE"
#endif
}

int fffs_index_set(struct fffs *fs, uint16_t slot, uint16_t head) {
    if (head == 0) {
        return fffs_index_remove(fs, slot);
    }
    return fffs_index_insert(fs, slot, head);
}
