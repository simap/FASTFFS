/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS compact RAM namespace index cache: stores authoritative
 * (resolved slot, head sector) mappings in an open-addressed hash table.
 */

#include "fffs_internal.h"

#include <string.h>

#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS

static bool is_power_of_two(size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

static struct fffs_index_bucket *buckets(struct fffs *fs) {
    return (struct fffs_index_bucket *)fs->index_cache;
}

static size_t bucket_for(struct fffs *fs, uint16_t slot) {
    return slot & (fs->index_hash_table_size - 1);
}

static size_t probe_distance(size_t mask, size_t from, size_t to) {
    return (to - from) & mask;
}

static int find_slot_bucket(struct fffs *fs, uint16_t slot,
        size_t *idx, bool *found) {
    if (!fs || slot == 0 || !idx || !found) {
        return FFFS_ERR_INVALID;
    }

    size_t mask = fs->index_hash_table_size - 1;
    size_t pos = bucket_for(fs, slot);
    for (size_t probe = 0; probe < fs->index_hash_table_size; probe++) {
        struct fffs_index_bucket *bucket = &buckets(fs)[pos];
        if (bucket->slot == 0) {
            *idx = pos;
            *found = false;
            return FFFS_OK;
        }
        if (bucket->slot == slot) {
            *idx = pos;
            *found = true;
            return FFFS_OK;
        }
        pos = (pos + 1) & mask;
    }

    *idx = 0;
    *found = false;
    return FFFS_ERR_NO_SPACE;
}

static void remove_bucket_at(struct fffs *fs, size_t remove_idx) {
    size_t mask = fs->index_hash_table_size - 1;
    size_t hole = remove_idx;
    size_t idx = (hole + 1) & mask;

    struct fffs_index_bucket *table = buckets(fs);
    while (table[idx].slot != 0) {
        uint16_t slot = table[idx].slot;
        size_t home = bucket_for(fs, slot);
        if (probe_distance(mask, home, idx) == 0) {
            break;
        }

        table[hole] = table[idx];
        hole = idx;
        idx = (idx + 1) & mask;
    }

    table[hole] = (struct fffs_index_bucket){0};
}

bool fffs_index_cache_config_valid(size_t count) {
    return is_power_of_two(count) &&
        count <= FFFS_INDEX_HASH_TABLE_SIZE_MAX;
}

size_t fffs_index_cache_required_size(size_t count) {
    return sizeof(struct fffs_index_bucket) * count;
}

int fffs_index_insert(struct fffs *fs, uint16_t slot, uint16_t head) {
    size_t idx;
    bool found;
    int err = find_slot_bucket(fs, slot, &idx, &found);
    if (err != FFFS_OK) {
        return err;
    }

    buckets(fs)[idx] = (struct fffs_index_bucket){
        .slot = slot,
        .head = head,
    };
    return FFFS_OK;
}

int fffs_index_remove(struct fffs *fs, uint16_t slot) {
    size_t idx;
    bool found;
    int err = find_slot_bucket(fs, slot, &idx, &found);
    if (err != FFFS_OK && err != FFFS_ERR_NO_SPACE) {
        return err;
    }
    if (!found) {
        return FFFS_OK;
    }

    remove_bucket_at(fs, idx);
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

    uint16_t candidate = fffs_normalize_slot_base(fffs_hash16(name));
    uint16_t first_free = 0;
    bool have_free = false;
    for (uint16_t d = 0; d <= FFFS_MAX_PROBE_DISTANCE; d++) {
        uint16_t candidate_head;
        bool occupied;
        int err = fffs_index_head_for_slot(fs, candidate,
                &candidate_head, &occupied);
        if (err != FFFS_OK) {
            return err;
        }

        if (!occupied) {
            if (!have_free) {
                first_free = candidate;
                have_free = true;
            }
            candidate = fffs_next_slot(candidate);
            continue;
        }

        struct fffs_stat st;
        uint16_t md_payload_data_off;
        uint16_t md_payload_data_len;
        uint16_t md_next;
        err = fffs_read_metadata_for_slot(fs, candidate_head, candidate, &st,
                &md_payload_data_off, &md_payload_data_len, &md_next, cache);
        if (err != FFFS_OK) {
            return err;
        }
        if (strcmp(st.name, name) == 0) {
            *slot = candidate;
            *head = candidate_head;
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
        struct fffs_index_bucket bucket =
            buckets(dir->fs)[dir->pos++];
        if (bucket.slot == 0) {
            continue;
        }

        struct fffs_stat candidate;
        int err = fffs_read_metadata_for_slot(dir->fs, bucket.head,
                bucket.slot, &candidate, NULL, NULL, NULL, NULL);
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

    size_t idx;
    bool slot_found;
    int err = find_slot_bucket(fs, slot, &idx, &slot_found);
    if (err != FFFS_OK && err != FFFS_ERR_NO_SPACE) {
        return err;
    }
    if (!slot_found) {
        return FFFS_OK;
    }

    *head = buckets(fs)[idx].head;
    *found = true;
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
        struct fffs_index_bucket bucket = buckets(fs)[i];
        uint16_t head = bucket.head;
        if (bucket.slot != 0 && head != 0) {
            fffs_alloc_map_mark_used(fs, head);
        }
    }
}

#endif
