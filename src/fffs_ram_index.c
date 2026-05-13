/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS full in-RAM namespace index cache: keeps one head sector for every
 * resolved slot and provides direct lookup, compaction, and liveness queries.
 */

#include "fffs_internal.h"

#include <string.h>

#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS

bool fffs_index_cache_config_valid(size_t count) {
    return count >= FFFS_SLOT_COUNT;
}

int fffs_index_candidate(struct fffs *fs, uint16_t slot, size_t probe,
        uint16_t *head, bool *end) {
    if (!fs || !head || !end) {
        return FFFS_ERR_INVALID;
    }
    *head = 0;
    *end = false;
    if (probe > 0 || fs->index_heads[slot] == 0) {
        *end = true;
        return FFFS_OK;
    }
    *head = fs->index_heads[slot];
    return FFFS_OK;
}

int fffs_index_insert(struct fffs *fs, uint16_t slot, uint16_t head) {
    fs->index_heads[slot] = head;
    return FFFS_OK;
}

int fffs_index_remove(struct fffs *fs, uint16_t slot) {
    fs->index_heads[slot] = 0;
    return FFFS_OK;
}

int fffs_index_set(struct fffs *fs, uint16_t slot, uint16_t head) {
    if (head == 0) {
        return fffs_index_remove(fs, slot);
    }
    return fffs_index_insert(fs, slot, head);
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

    uint16_t candidate = fffs_normalize_slot_base(fffs_hash16(name));
    uint16_t first_free = 0;
    bool have_free = false;
    for (uint16_t d = 0; d <= FFFS_MAX_PROBE_DISTANCE; d++) {
        uint16_t candidate_head = fs->index_heads[candidate];
        if (candidate_head == 0) {
            if (!have_free) {
                first_free = candidate;
                have_free = true;
            }
            candidate = fffs_next_slot(candidate);
            continue;
        }

        struct fffs_stat st;
        uint16_t md_slot;
        uint16_t md_data_off;
        uint16_t md_data_len;
        uint16_t md_next;
        int err = fffs_read_metadata(fs, candidate_head, &st, &md_slot,
                &md_data_off, &md_data_len, &md_next);
        if (err == FFFS_ERR_CORRUPT) {
            candidate = fffs_next_slot(candidate);
            continue;
        }
        if (err != FFFS_OK) {
            return err;
        }
        if (md_slot == candidate && strcmp(st.name, name) == 0) {
            *slot = candidate;
            *head = candidate_head;
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
        candidate = fffs_next_slot(candidate);
    }

    if (!have_free) {
        return FFFS_ERR_NO_SPACE;
    }
    *slot = first_free;
    *head = 0;
    *found = false;
    return FFFS_OK;
}

bool fffs_index_dir_read(struct fffs_dir *dir, struct fffs_stat *st) {
    while (dir->pos < FFFS_SLOT_COUNT) {
        uint16_t head = dir->fs->index_heads[dir->pos++];
        if (head == 0) {
            continue;
        }

        struct fffs_stat candidate;
        int err = fffs_read_metadata(dir->fs, head, &candidate,
                NULL, NULL, NULL, NULL);
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

int fffs_index_compact(struct fffs *fs, size_t *offset, size_t sector_end) {
    for (size_t slot = 0; slot < FFFS_SLOT_COUNT; slot++) {
        uint16_t head = fs->index_heads[slot];
        if (head == 0) {
            continue;
        }
        int err = fffs_compact_index_entry(fs, offset, (uint16_t)slot,
                head, sector_end);
        if (err != FFFS_OK) {
            return err;
        }
    }
    return FFFS_OK;
}

bool fffs_index_sector_is_live_head(struct fffs *fs, size_t sector) {
    for (size_t i = 0; i < FFFS_SLOT_COUNT; i++) {
        if (fs->index_heads[i] == sector) {
            return true;
        }
    }
    return false;
}

int fffs_index_sector_is_live_extent(struct fffs *fs, size_t sector,
        bool *reachable) {
    *reachable = false;
    for (size_t i = 0; i < FFFS_SLOT_COUNT; i++) {
        uint16_t head = fs->index_heads[i];
        if (head == 0) {
            continue;
        }
        uint16_t current = head;
        for (size_t depth = 0; current != 0 && depth < fs->sector_count;
                depth++) {
            if (current == sector) {
                *reachable = true;
                return FFFS_OK;
            }
            uint16_t next_sector;
            int err = fffs_read_metadata(fs, current, NULL, NULL, NULL,
                    NULL, &next_sector);
            if (err != FFFS_OK) {
                return err;
            }
            current = next_sector;
        }
        if (current != 0) {
            return FFFS_ERR_CORRUPT;
        }
    }
    return FFFS_OK;
}

#endif
