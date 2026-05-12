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
#endif

bool fffs_index_head_count_valid(size_t count) {
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
    return count >= FFFS_SLOT_COUNT;
#elif FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS
    return is_power_of_two(count) &&
        count <= FFFS_INDEX_HASH_HEAD_COUNT_MAX;
#else
#error "Unsupported FFFS_INDEX_CACHE_MODE"
#endif
}

int fffs_index_find(struct fffs *fs, uint16_t slot, uint16_t *head) {
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
    uint16_t h = fs->index_heads[slot];
    if (h == 0) {
        return FFFS_ERR_NOT_FOUND;
    }
    *head = h;
    return FFFS_OK;
#elif FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS
    size_t mask = fs->index_head_count - 1;
    size_t idx = slot & mask;
    for (size_t probe = 0; probe < fs->index_head_count; probe++) {
        uint16_t h = fs->index_heads[idx];
        if (h == 0) {
            return FFFS_ERR_NOT_FOUND;
        }
        uint16_t md_slot;
        int err = fffs_read_metadata(fs, h, NULL, &md_slot, NULL, NULL);
        if (err != FFFS_OK) {
            return err;
        }
        if (md_slot == slot) {
            *head = h;
            return FFFS_OK;
        }
        idx = (idx + 1) & mask;
    }
    return FFFS_ERR_CORRUPT;
#else
#error "Unsupported FFFS_INDEX_CACHE_MODE"
#endif
}

int fffs_index_insert(struct fffs *fs, uint16_t slot, uint16_t head) {
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
    fs->index_heads[slot] = head;
    return FFFS_OK;
#elif FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS
    size_t mask = fs->index_head_count - 1;
    size_t idx = slot & mask;
    for (size_t probe = 0; probe < fs->index_head_count; probe++) {
        uint16_t h = fs->index_heads[idx];
        if (h == 0) {
            fs->index_heads[idx] = head;
            return FFFS_OK;
        }
        uint16_t md_slot;
        int err = fffs_read_metadata(fs, h, NULL, &md_slot, NULL, NULL);
        if (err != FFFS_OK) {
            return err;
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

int fffs_index_remove(struct fffs *fs, uint16_t slot) {
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
    fs->index_heads[slot] = 0;
    return FFFS_OK;
#elif FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS
    size_t mask = fs->index_head_count - 1;
    size_t idx = slot & mask;
    for (size_t probe = 0; probe < fs->index_head_count; probe++) {
        uint16_t h = fs->index_heads[idx];
        if (h == 0) {
            return FFFS_OK;
        }
        uint16_t md_slot;
        int err = fffs_read_metadata(fs, h, NULL, &md_slot, NULL, NULL);
        if (err != FFFS_OK) {
            return err;
        }
        if (md_slot == slot) {
            fs->index_heads[idx] = 0;
            idx = (idx + 1) & mask;
            while (fs->index_heads[idx] != 0) {
                uint16_t reinsert_head = fs->index_heads[idx];
                fs->index_heads[idx] = 0;
                uint16_t reinsert_slot;
                err = fffs_read_metadata(fs, reinsert_head, NULL,
                        &reinsert_slot, NULL, NULL);
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
