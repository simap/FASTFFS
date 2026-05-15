/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS compact RAM namespace index cache: stores only head sectors in an
 * open-addressed hash table and resolves slot identity from root metadata when
 * collisions or stale heads require it.
 */

#include "fffs_internal.h"

#include <string.h>

#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS

#define FFFS_INDEX_STALE_HEAD 1u

static bool is_power_of_two(size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

static int hash_remove_at(struct fffs *fs, size_t remove_idx);
static int hash_repair_from(struct fffs *fs, size_t hole);
static bool bucket_can_hold_slot(struct fffs *fs, size_t idx,
        uint16_t slot);
static int index_candidate(struct fffs *fs, uint16_t slot, size_t probe,
        uint16_t *head, bool *end);

bool fffs_index_cache_config_valid(size_t count) {
    return is_power_of_two(count) &&
        count <= FFFS_INDEX_HASH_TABLE_SIZE_MAX;
}

static int index_candidate(struct fffs *fs, uint16_t slot, size_t probe,
        uint16_t *head, bool *end) {
    if (!fs || !head || !end) {
        return FFFS_ERR_INVALID;
    }
    *head = 0;
    *end = false;
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
}

int fffs_index_insert(struct fffs *fs, uint16_t slot, uint16_t head) {
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
        if (!bucket_can_hold_slot(fs, idx, md_slot)) {
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
}

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

static bool first_stale_in_probe_path(struct fffs *fs, size_t home,
        size_t idx, size_t *stale) {
    size_t mask = fs->index_hash_table_size - 1;
    size_t pos = home;
    for (size_t scanned = 0; scanned < fs->index_hash_table_size &&
            pos != idx; scanned++) {
        uint16_t h = fs->index_heads[pos];
        if (h == FFFS_INDEX_STALE_HEAD) {
            *stale = pos;
            return true;
        }
        if (h == 0) {
            return false;
        }
        pos = (pos + 1) & mask;
    }
    return false;
}

static int hash_repair_from(struct fffs *fs, size_t hole) {
    size_t mask = fs->index_hash_table_size - 1;

    size_t start = hole;
    for (size_t scanned = 0; scanned + 1 < fs->index_hash_table_size;
            scanned++) {
        size_t prev = (start - 1) & mask;
        if (fs->index_heads[prev] == 0) {
            break;
        }
        start = prev;
    }

    size_t repair_len = 0;
    size_t idx = start;
    while (repair_len < fs->index_hash_table_size &&
            fs->index_heads[idx] != 0) {
        repair_len++;
        idx = (idx + 1) & mask;
    }

    for (size_t pass = 0; pass < repair_len; pass++) {
        bool moved = false;
        idx = start;
        for (size_t scanned = 0; scanned < repair_len; scanned++) {
            uint16_t h = fs->index_heads[idx];
            if (h == 0) {
                break;
            }
            if (h == FFFS_INDEX_STALE_HEAD) {
                idx = (idx + 1) & mask;
                continue;
            }

            uint16_t slot;
            int err = fffs_read_metadata(fs, h, NULL, &slot, NULL, NULL,
                    NULL);
            if (err != FFFS_OK) {
                if (err != FFFS_ERR_CORRUPT) {
                    return err;
                }
                fs->index_heads[idx] = FFFS_INDEX_STALE_HEAD;
                moved = true;
                idx = (idx + 1) & mask;
                continue;
            }

            if (!bucket_can_hold_slot(fs, idx, slot)) {
                fs->index_heads[idx] = FFFS_INDEX_STALE_HEAD;
                moved = true;
                idx = (idx + 1) & mask;
                continue;
            }

            size_t stale;
            size_t home = slot & mask;
            if (first_stale_in_probe_path(fs, home, idx, &stale)) {
                fs->index_heads[stale] = h;
                fs->index_heads[idx] = FFFS_INDEX_STALE_HEAD;
                moved = true;
            }
            idx = (idx + 1) & mask;
        }
        if (!moved) {
            break;
        }
    }

    idx = start;
    for (size_t scanned = 0; scanned < repair_len; scanned++) {
        if (fs->index_heads[idx] == FFFS_INDEX_STALE_HEAD) {
            fs->index_heads[idx] = 0;
        }
        idx = (idx + 1) & mask;
    }
    return FFFS_OK;
}

int fffs_index_remove(struct fffs *fs, uint16_t slot) {
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
        bool occupied = false;
        for (size_t probe = 0; probe < fs->index_hash_table_size;) {
            uint16_t candidate_head;
            bool end;
            int err = index_candidate(fs, candidate, probe,
                    &candidate_head, &end);
            if (err != FFFS_OK) {
                return err;
            }
            if (end) {
                break;
            }

            struct fffs_stat st;
            uint16_t md_slot;
            uint16_t md_data_off;
            uint16_t md_data_len;
            uint16_t md_next;
            err = fffs_read_metadata(fs, candidate_head, &st, &md_slot,
                    &md_data_off, &md_data_len, &md_next);
            if (err == FFFS_ERR_CORRUPT) {
                occupied = true;
                probe++;
                continue;
            }
            if (err != FFFS_OK) {
                return err;
            }
            if (md_slot == candidate) {
                occupied = true;
                if (strcmp(st.name, name) == 0) {
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
            }
            probe++;
        }
        if (!occupied && !have_free) {
            first_free = candidate;
            have_free = true;
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
    while (dir->pos < dir->fs->index_hash_table_size) {
        uint16_t head = dir->fs->index_heads[dir->pos++];
        if (head == 0 || head == FFFS_INDEX_STALE_HEAD) {
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

int fffs_index_head_for_slot(struct fffs *fs, uint16_t slot,
        uint16_t *head, bool *found) {
    *head = 0;
    *found = false;
    for (size_t probe = 0; probe < fs->index_hash_table_size; probe++) {
        uint16_t candidate_head;
        bool end;
        int err = index_candidate(fs, slot, probe, &candidate_head,
                &end);
        if (err != FFFS_OK || end) {
            return err;
        }

        uint16_t md_slot;
        err = fffs_read_metadata(fs, candidate_head, NULL, &md_slot, NULL,
                NULL, NULL);
        if (err == FFFS_ERR_CORRUPT) {
            continue;
        }
        if (err != FFFS_OK) {
            return err;
        }
        if (md_slot == slot) {
            *head = candidate_head;
            *found = true;
            return FFFS_OK;
        }
    }
    return FFFS_OK;
}

int fffs_index_record_is_current(struct fffs *fs,
        size_t seq_pos, size_t offset, uint16_t slot, uint16_t head,
        bool *current) {
    (void)seq_pos;
    (void)offset;
    uint16_t current_head;
    bool found;
    int err = fffs_index_head_for_slot(fs, slot, &current_head, &found);
    if (err != FFFS_OK) {
        return err;
    }
    *current = found && current_head == head;
    return FFFS_OK;
}

void fffs_index_mark_live_heads_used(struct fffs *fs) {
    for (size_t i = 0; i < fs->index_hash_table_size; i++) {
        uint16_t head = fs->index_heads[i];
        if (head != 0 && head != FFFS_INDEX_STALE_HEAD) {
            fffs_alloc_map_mark_used(fs, head);
        }
    }
}

#endif
