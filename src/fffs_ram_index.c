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
static bool is_power_of_two(size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

static int hash_remove_at(struct fffs *fs, size_t remove_idx);
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
    if (h == 0) {
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
static int hash_remove_at(struct fffs *fs, size_t remove_idx) {
    size_t mask = fs->index_hash_table_size - 1;
    fs->index_heads[remove_idx] = 0;
    size_t idx = (remove_idx + 1) & mask;
    while (fs->index_heads[idx] != 0) {
        uint16_t reinsert_head = fs->index_heads[idx];
        fs->index_heads[idx] = 0;
        uint16_t reinsert_slot;
        int err = fffs_read_metadata(fs, reinsert_head, NULL,
                &reinsert_slot, NULL, NULL, NULL);
        if (err == FFFS_ERR_CORRUPT) {
            idx = (idx + 1) & mask;
            continue;
        }
        if (err != FFFS_OK) {
            return err;
        }
        err = fffs_index_insert(fs, reinsert_slot, reinsert_head);
        if (err != FFFS_OK) {
            return err;
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
restart:
    for (size_t probe = 0; probe < fs->index_hash_table_size; probe++) {
        uint16_t h = fs->index_heads[idx];
        if (h == 0) {
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
            return hash_remove_at(fs, idx);
        }
        idx = (idx + 1) & mask;
    }
    return FFFS_OK;
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
