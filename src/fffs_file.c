/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS file-handle operations: open/read/seek/write/close, streaming
 * cursor state, inflight writer checks, and contiguous span accumulation.
 */

#include "fffs_internal.h"

#include <stdbool.h>
#include <string.h>

bool fffs_invalidate_old_chain(struct fffs *fs, uint16_t slot,
        uint16_t head, uint16_t next, bool tombstone,
        bool *root_accounted) {
    uint16_t current = head;
    uint16_t current_next = next;
    bool have_next = true;
    size_t visited = 0;
    if (root_accounted) {
        *root_accounted = false;
    }

    for (size_t depth = 0; current != 0 && depth < fs->sector_count; depth++) {
        struct fffs_md_record record;
        int err = fffs_read_md_for_slot(fs, current, slot, &record);
        if (err != FFFS_OK) {
            return false;
        }
        if (!have_next) {
            current_next = record.next;
        }
        if ((size_t)current + record.span_len > fs->sector_count) {
            return false;
        }
        fffs_alloc_map_mark_range_unknown(fs, current, record.span_len);
        visited += record.span_len;
#if !FFFS_LAZY_DELETE_TOMBSTONES
        if (tombstone) {
            for (uint16_t i = 0; i < record.span_len; i++) {
                bool accounted = false;
                enum fffs_tombstone_accounting accounting =
                    depth == 0 && i == 0 ?
                    FFFS_TOMBSTONE_COMMITTED_DELETE :
                    FFFS_TOMBSTONE_NO_ACCOUNTING;
                int err = fffs_tombstone_metadata_for_slot(fs,
                        (uint16_t)(current + i), slot, accounting,
                        accounting == FFFS_TOMBSTONE_COMMITTED_DELETE ?
                        &accounted : NULL);
                if (err != FFFS_OK) {
                    return false;
                }
                if (accounting == FFFS_TOMBSTONE_COMMITTED_DELETE &&
                        root_accounted) {
                    *root_accounted = accounted;
                }
            }
        }
#else
        (void)tombstone;
#endif
        current = current_next;
        have_next = false;
    }
    if (current != 0) {
        return false;
    }
    if (visited > 0) {
        uint32_t sample = (uint32_t)(visited * FFFS_SECTORS_PER_FILE_Q8_ONE);
        fs->avg_sectors_per_file_q8 =
            (uint16_t)(((uint32_t)fs->avg_sectors_per_file_q8 * 7u +
                    sample) / 8u);
        fs->fsinfo_valid_flags &= ~FFFS_FSINFO_METADATA_ESTIMATE_VALID;
    }
    return true;
}

static uint32_t claim_sector_serial(struct fffs *fs) {
    uint32_t serial = fs->next_sector_serial++;
    if (fs->next_sector_serial == 0) {
        fs->next_sector_serial = 1;
    }
    return serial;
}

static void register_inflight_writer(struct fffs_file *file) {
    if (file->inflight_registered) {
        return;
    }
    file->inflight_next = file->fs->inflight_writers;
    file->fs->inflight_writers = file;
    file->inflight_registered = true;
}

static void unregister_inflight_writer(struct fffs_file *file) {
    if (!file->inflight_registered || !file->fs) {
        return;
    }
    struct fffs_file **p = &file->fs->inflight_writers;
    while (*p) {
        if (*p == file) {
            *p = file->inflight_next;
            break;
        }
        p = &(*p)->inflight_next;
    }
    file->inflight_next = NULL;
    file->inflight_registered = false;
}

static void stage_root_prefix(struct fffs_file *file, const char *name) {
    size_t name_len = strlen(name);
    file->root_payload_offset = (uint16_t)(1u + name_len + 1u);
    file->cache[0] = (uint8_t)name_len;
    memcpy(file->cache + 1, name, name_len);
    file->cache[1u + name_len] = 0;
    file->cache_len = file->root_payload_offset;
}

static size_t current_extent_capacity(const struct fffs_file *file) {
    size_t data_off = file->data_offset;
    size_t record_off = file->current_metadata_offset;
    size_t prefix = file->current == file->head ?
        file->root_payload_offset : 0;
    if (record_off <= data_off + prefix) {
        return 0;
    }
    return record_off - data_off - prefix;
}

static uint16_t pending_span_len(const struct fffs_file *file) {
    return (uint16_t)(file->current - file->span_head + 1u);
}

bool fffs_sector_is_inflight(struct fffs *fs, uint16_t sector) {
    for (struct fffs_file *file = fs->inflight_writers; file;
            file = file->inflight_next) {
        if (file->closed || (file->flags & FFFS_O_WRONLY) == 0) {
            continue;
        }
        if (file->head == sector || file->current == sector) {
            return true;
        }
        if (file->span_head != 0 && sector >= file->span_head &&
                sector <= file->current) {
            return true;
        }
    }
    return false;
}

bool fffs_slot_is_inflight(struct fffs *fs, uint16_t slot) {
    for (struct fffs_file *file = fs->inflight_writers; file;
            file = file->inflight_next) {
        if (file->closed || (file->flags & FFFS_O_WRONLY) == 0) {
            continue;
        }
        if (file->slot == slot) {
            return true;
        }
    }
    return false;
}

int fffs_open(struct fffs *fs, struct fffs_file *file,
        const char *name, uint32_t flags) {
    if (!fs || !file || !name) {
        return FFFS_ERR_INVALID;
    }
    *file = (struct fffs_file){0};
    bool read = (flags & FFFS_O_RDONLY) != 0;
    bool write = (flags & FFFS_O_WRONLY) != 0;
    if (read == write || (flags & ~(uint32_t)(FFFS_O_RDONLY |
                    FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC |
                    FFFS_O_EXCL)) != 0) {
        return FFFS_ERR_INVALID;
    }

    uint16_t slot;
    uint16_t head;
    bool found;
    struct fffs_stat resolved_st;
    uint16_t resolved_payload_data_off = 0;
    uint16_t resolved_payload_data_len = 0;
    uint16_t resolved_next = 0;
    uint16_t resolved_span_len = 1;
    struct fffs_read_cache_view read_cache = {
        .data = file->cache,
        .capacity = sizeof(file->cache),
    };
    FFFS_PROFILE_PUSH(fs, FFFS_PROFILE_INDEX_RESOLVE);
    int err = fffs_index_resolve(fs, name, &slot, &head, &found, &resolved_st,
            &resolved_payload_data_off, &resolved_payload_data_len,
            &resolved_next, read ? &read_cache : NULL);
    FFFS_PROFILE_POP(fs, FFFS_PROFILE_INDEX_RESOLVE);
    if (err != FFFS_OK) {
        return err;
    }
    if (read && !found) {
        return FFFS_ERR_NOT_FOUND;
    }
    if (write && !found && (flags & FFFS_O_CREATE) == 0) {
        return FFFS_ERR_NOT_FOUND;
    }
    if (write && found && (flags & FFFS_O_EXCL) != 0) {
        return FFFS_ERR_EXISTS;
    }
    if (write && found && (flags & FFFS_O_TRUNC) == 0) {
        return FFFS_ERR_EXISTS;
    }

    file->fs = fs;
    file->flags = flags;
    file->slot = slot;
    file->head = head;

    if (read) {
        struct fffs_md_record root;
        err = fffs_read_md_for_slot(fs, head, slot, &root);
        if (err != FFFS_OK) {
            return err;
        }
        resolved_span_len = root.span_len;
        file->cache_len = read_cache.len;
        file->cache_data_pos = read_cache.data_pos;
        file->data_offset = resolved_payload_data_off;
        file->size = resolved_st.size;
        file->current = head;
        file->current_data_len = resolved_payload_data_len;
        file->span_len = resolved_span_len;
        file->current_next = resolved_next;
        memcpy(file->name, resolved_st.name, strlen(resolved_st.name) + 1);
    } else {
        uint16_t sector;
        if (found) {
            file->old_head = head;
            file->old_next = resolved_next;
        }
        err = fffs_alloc_next_sector(file, &sector);
        if (err != FFFS_OK) {
            return err;
        }
        file->head = sector;
        file->current = sector;
        if (file->current_needs_footer) {
            file->current_sector_serial = claim_sector_serial(file->fs);
        } else {
            err = fffs_read_sector_serial(file->fs, file->current,
                    &file->current_sector_serial);
            if (err != FFFS_OK) {
                return err;
            }
        }
        memcpy(file->name, name, strlen(name) + 1);
        stage_root_prefix(file, name);
        file->span_head = file->current;
        file->span_len = 0;
        register_inflight_writer(file);
    }

    return FFFS_OK;
}

static int reset_read_position_to_root(struct fffs_file *file) {
    uint16_t data_len;
    uint16_t next;
    uint16_t span_len;
    int err = fffs_read_file_root_md(file->fs, file->head, file->slot,
            NULL, &file->data_offset, &data_len, &next, &span_len, NULL,
            NULL);
    if (err != FFFS_OK) {
        return err;
    }
    file->current = file->head;
    file->current_data_len = data_len;
    file->span_len = span_len;
    file->current_next = next;
    file->current_data_pos = 0;
    file->current_file_offset = 0;
    file->cache_len = 0;
    file->cache_data_pos = 0;
    return FFFS_OK;
}

static int hunt_current_span(struct fffs_file *file, uint32_t target,
        bool *found);

static int seek_from_current_position(struct fffs_file *file,
        uint32_t target) {
    size_t depth = 0;
    while (target > file->current_file_offset + file->current_data_len) {
        bool found_in_span = false;
        int hunt_err = hunt_current_span(file, target, &found_in_span);
        if (hunt_err != FFFS_OK || found_in_span) {
            return hunt_err;
        }
        uint16_t next_sector = file->span_len > 1 ?
            (uint16_t)(file->current + 1u) : file->current_next;
        if (next_sector == 0 || ++depth >= file->fs->sector_count) {
            return FFFS_ERR_CORRUPT;
        }
        uint32_t next_file_offset =
            file->current_file_offset + file->current_data_len;
        uint16_t data_len;
        uint16_t next;
        uint16_t span_len;
        struct fffs_md_record record;
        int err = fffs_read_md_for_slot(file->fs, next_sector, file->slot,
                &record);
        if (err != FFFS_OK) {
            return err;
        }
        file->data_offset = record.data_off;
        data_len = record.data_len;
        next = record.next;
        span_len = record.span_len;
        file->current = next_sector;
        file->current_data_len = data_len;
        file->span_len = span_len;
        file->current_next = next;
        file->current_file_offset = next_file_offset;
        file->current_data_pos = 0;
        file->cache_len = 0;
    }

    file->current_data_pos = target - file->current_file_offset;
    file->pos = target;
    return FFFS_OK;
}

static int hunt_current_span(struct fffs_file *file, uint32_t target,
        bool *found) {
    *found = false;
    if (file->span_len <= 1 ||
            target <= file->current_file_offset + file->current_data_len) {
        return FFFS_OK;
    }

    uint16_t low = 1;
    uint16_t high = (uint16_t)(file->span_len - 1u);
    uint32_t rel = target - file->current_file_offset;
    uint32_t estimate = (uint32_t)fffs_max_file_data_size(file->fs);
    if (estimate == 0) {
        estimate = 1;
    }

    while (low <= high) {
        uint16_t guess = (uint16_t)(rel / estimate);
        if (guess < low) {
            guess = low;
        }
        if (guess > high) {
            guess = high;
        }

        uint16_t sector = (uint16_t)(file->current + guess);
        struct fffs_md_record record;
        int err = fffs_read_md_for_slot(file->fs, sector, file->slot, &record);
        if (err != FFFS_OK) {
            return err;
        }
        uint32_t file_offset = record.type == FFFS_MD_TYPE_FILE_ROOT_V1 ?
            0 : record.size_or_offset;
        if (target < file_offset) {
            if (guess == 0) {
                break;
            }
            high = (uint16_t)(guess - 1u);
        } else if (target <= file_offset + record.data_len) {
            file->current = sector;
            file->data_offset = record.data_off;
            file->current_data_len = record.data_len;
            file->span_len = record.span_len;
            file->current_next = record.next;
            file->current_file_offset = file_offset;
            file->current_data_pos = target - file_offset;
            file->pos = target;
            file->cache_len = 0;
            *found = true;
            return FFFS_OK;
        } else {
            low = (uint16_t)(guess + 1u);
        }
    }
    return FFFS_OK;
}

static int seek_read_position(struct fffs_file *file, uint32_t target) {
    if (target < file->current_file_offset) {
        int err = reset_read_position_to_root(file);
        if (err != FFFS_OK) {
            return err;
        }
    }
    return seek_from_current_position(file, target);
}

static bool read_cache_has_remaining(const struct fffs_file *file) {
    return file->cache_len != 0 &&
        file->current_data_pos >= file->cache_data_pos &&
        file->current_data_pos < file->cache_data_pos + file->cache_len;
}

static int fill_read_cache(struct fffs_file *file) {
    size_t remaining = file->current_data_len - file->current_data_pos;
    size_t n = remaining < sizeof(file->cache) ? remaining :
        sizeof(file->cache);
    int err = fffs_flash_read(file->fs, (size_t)file->current *
            file->fs->sector_size + file->data_offset +
            file->current_data_pos, file->cache, n);
    if (err != FFFS_OK) {
        return err;
    }
    file->cache_data_pos = file->current_data_pos;
    file->cache_len = n;
    return FFFS_OK;
}

int fffs_read(struct fffs_file *file, void *buffer, size_t size,
        size_t *out_read) {
    if (!file || file->closed || (!buffer && size) ||
            (file->flags & FFFS_O_RDONLY) == 0) {
        return FFFS_ERR_INVALID;
    }
    size_t total = 0;
    uint8_t *dst = buffer;
    size_t remaining = file->size - file->pos;
    size_t want = size < remaining ? size : remaining;
    if (out_read) {
        *out_read = 0;
    }
    if (want == 0) {
        return FFFS_OK;
    }

    FFFS_PROFILE_PUSH(file->fs, FFFS_PROFILE_READ);
    while (total < want) {
        if (file->current_data_pos >= file->current_data_len) {
            uint16_t next_sector = file->span_len > 1 ?
                (uint16_t)(file->current + 1u) : file->current_next;
            if (next_sector == 0) {
                FFFS_PROFILE_POP(file->fs, FFFS_PROFILE_READ);
                return FFFS_ERR_CORRUPT;
            }
            uint32_t next_file_offset =
                file->current_file_offset + file->current_data_len;
            uint16_t data_len;
            uint16_t next;
            uint16_t span_len;
            struct fffs_md_record record;
            int err = fffs_read_md_for_slot(file->fs, next_sector,
                    file->slot, &record);
            if (err != FFFS_OK) {
                FFFS_PROFILE_POP(file->fs, FFFS_PROFILE_READ);
                return err;
            }
            file->data_offset = record.data_off;
            data_len = record.data_len;
            next = record.next;
            span_len = record.span_len;
            file->current = next_sector;
            file->current_data_len = data_len;
            file->span_len = span_len;
            file->current_next = next;
            file->current_data_pos = 0;
            file->current_file_offset = next_file_offset;
            file->cache_len = 0;
        }

        size_t in_current_data = file->current_data_len -
            file->current_data_pos;
        size_t n = want - total < in_current_data ? want - total :
            in_current_data;
        if (read_cache_has_remaining(file)) {
            size_t cache_pos = file->current_data_pos - file->cache_data_pos;
            size_t cached = file->cache_len - cache_pos;
            if (n > cached) {
                n = cached;
            }
            memcpy(dst + total, file->cache + cache_pos, n);
        } else if (n < sizeof(file->cache)) {
            int err = fill_read_cache(file);
            if (err != FFFS_OK) {
                FFFS_PROFILE_POP(file->fs, FFFS_PROFILE_READ);
                return err;
            }
            n = want - total < file->cache_len ? want - total :
                file->cache_len;
            memcpy(dst + total, file->cache, n);
        } else {
            file->cache_len = 0;
            int err = fffs_flash_read(file->fs, (size_t)file->current *
                    file->fs->sector_size + file->data_offset +
                    file->current_data_pos, dst + total, n);
            if (err != FFFS_OK) {
                FFFS_PROFILE_POP(file->fs, FFFS_PROFILE_READ);
                return err;
            }
        }
        file->current_data_pos += (uint32_t)n;
        file->pos += (uint32_t)n;
        total += n;
    }
    if (out_read) {
        *out_read = total;
    }
    FFFS_PROFILE_POP(file->fs, FFFS_PROFILE_READ);
    return FFFS_OK;
}

int fffs_seek(struct fffs_file *file, int32_t offset,
        enum fffs_seek_whence whence, uint32_t *out_pos) {
    if (!file || file->closed || (file->flags & FFFS_O_RDONLY) == 0) {
        return FFFS_ERR_INVALID;
    }

    int64_t base;
    switch (whence) {
    case FFFS_SEEK_SET:
        base = 0;
        break;
    case FFFS_SEEK_CUR:
        base = file->pos;
        break;
    case FFFS_SEEK_END:
        base = file->size;
        break;
    default:
        return FFFS_ERR_INVALID;
    }

    int64_t target = base + offset;
    if (target < 0 || target > (int64_t)file->size) {
        return FFFS_ERR_RANGE;
    }

    int err = seek_read_position(file, (uint32_t)target);
    if (err != FFFS_OK) {
        return err;
    }
    if (out_pos) {
        *out_pos = file->pos;
    }
    return FFFS_OK;
}

static int flush_write_cache(struct fffs_file *file, bool final) {
    size_t granule = file->fs->backend.program_granule;
    if (file->cache_len == 0) {
        return FFFS_OK;
    }
    if (!final && file->cache_len < granule) {
        return FFFS_OK;
    }
    size_t n = file->cache_len - (file->cache_len % granule);
    if (final && n != file->cache_len) {
        n = ((file->cache_len + granule - 1) / granule) * granule;
        memset(file->cache + file->cache_len, 0xff, n - file->cache_len);
    }
    if (n == 0) {
        return FFFS_OK;
    }
    int err = fffs_flash_program_aligned(file->fs, (size_t)file->current *
            file->fs->sector_size + file->current_write_offset,
            file->cache, n);
    if (err != FFFS_OK) {
        return err;
    }
    file->current_write_offset += (uint16_t)n;
    if (n < file->cache_len) {
        memmove(file->cache, file->cache + n, file->cache_len - n);
    }
    file->cache_len -= n < file->cache_len ? n : file->cache_len;
    return FFFS_OK;
}

static int flush_pending_span_head(struct fffs_file *file, uint16_t next_sector,
        bool final) {
    if (file->span_head == 0) {
        return FFFS_OK;
    }

    bool is_root = file->span_head == file->head;
    uint16_t span_len = pending_span_len(file);
    if (is_root && !final) {
        int err = fffs_finish_extent_metadata(file, file->span_head,
                next_sector, span_len, 0, false, false, false);
        if (err != FFFS_OK) {
            return err;
        }
        file->span_head = 0;
        return FFFS_OK;
    }

    int err = fffs_finish_extent_metadata(file, file->span_head,
            next_sector, span_len, file->size, is_root, true, is_root);
    if (err != FFFS_OK) {
        return err;
    }
    file->span_head = 0;
    return FFFS_OK;
}

static int start_next_extent(struct fffs_file *file) {
    int err = flush_write_cache(file, true);
    if (err != FFFS_OK) {
        return err;
    }

    uint16_t old_sector = file->current;
    uint16_t old_data_len = file->current_data_len;
    uint16_t old_data_offset = file->data_offset;
    uint16_t old_metadata_offset = file->current_metadata_offset;
    bool old_needs_footer = file->current_needs_footer;
    uint32_t old_sector_serial = file->current_sector_serial;
    uint32_t old_file_offset = file->current_file_offset;

    uint16_t next_sector;
    err = fffs_alloc_next_sector(file, &next_sector);
    if (err != FFFS_OK) {
        return err;
    }

    bool can_extend_span = (size_t)next_sector == (size_t)old_sector + 1u &&
        pending_span_len(file) < UINT16_MAX;
    uint32_t next_file_offset = old_file_offset + old_data_len;
    if (old_sector == file->span_head) {
        err = fffs_begin_extent_metadata(file, old_sector, old_sector_serial,
                old_data_offset, old_metadata_offset, old_needs_footer,
                old_data_len, old_file_offset);
        if (err != FFFS_OK) {
            return err;
        }
    } else {
        err = fffs_write_extent_metadata(file, old_sector, old_sector_serial,
                old_data_offset, old_metadata_offset, old_needs_footer,
                old_data_len, 1, 0, next_sector, old_file_offset, false);
        if (err != FFFS_OK) {
            return err;
        }
    }
    if (!can_extend_span) {
        err = flush_pending_span_head(file, next_sector, false);
        if (err != FFFS_OK) {
            return err;
        }
    }

    file->current = next_sector;
    if (file->current_needs_footer) {
        file->current_sector_serial = claim_sector_serial(file->fs);
    } else {
        err = fffs_read_sector_serial(file->fs, file->current,
                &file->current_sector_serial);
        if (err != FFFS_OK) {
            return err;
        }
    }
    file->current_data_len = 0;
    file->current_next = 0;
    file->current_file_offset = next_file_offset;
    file->cache_len = 0;
    if (!can_extend_span) {
        file->span_head = file->current;
        file->span_len = 0;
    }
    return FFFS_OK;
}

int fffs_write(struct fffs_file *file, const void *buffer, size_t size) {
    if (!file || file->closed || (!buffer && size) ||
            (file->flags & FFFS_O_WRONLY) == 0) {
        return FFFS_ERR_INVALID;
    }
    const uint8_t *src = buffer;
    size_t remaining = size;
    FFFS_PROFILE_PUSH(file->fs, FFFS_PROFILE_WRITE);
    while (remaining > 0) {
        size_t capacity = current_extent_capacity(file);
        if (file->current_data_len >= capacity) {
            int err = start_next_extent(file);
            if (err != FFFS_OK) {
                FFFS_PROFILE_POP(file->fs, FFFS_PROFILE_WRITE);
                return err;
            }
            capacity = current_extent_capacity(file);
        }

        size_t extent_space = capacity - file->current_data_len;
        size_t buffer_space = sizeof(file->cache) - file->cache_len;
        size_t space = extent_space < buffer_space ? extent_space :
            buffer_space;
        size_t n = remaining < space ? remaining : space;
        memcpy(file->cache + file->cache_len, src, n);
        file->cache_len += n;
        file->current_data_len += (uint16_t)n;
        file->size += (uint32_t)n;
        src += n;
        remaining -= n;
        if (file->cache_len == sizeof(file->cache)) {
            int err = flush_write_cache(file, false);
            if (err != FFFS_OK) {
                FFFS_PROFILE_POP(file->fs, FFFS_PROFILE_WRITE);
                return err;
            }
        }
    }
    FFFS_PROFILE_POP(file->fs, FFFS_PROFILE_WRITE);
    return FFFS_OK;
}

int fffs_fstat(struct fffs_file *file, struct fffs_stat *st) {
    if (!file || file->closed || !st) {
        return FFFS_ERR_INVALID;
    }
    memset(st, 0, sizeof(*st));
    if ((file->flags & FFFS_O_WRONLY) != 0) {
        memcpy(st->name, file->name, strlen(file->name) + 1);
        st->size = file->size;
        return FFFS_OK;
    }
    return fffs_read_file_root_md(file->fs, file->head, file->slot, st,
            NULL, NULL, NULL, NULL, NULL, NULL);
}

int fffs_close(struct fffs_file *file) {
    if (!file || file->closed) {
        return FFFS_ERR_INVALID;
    }
    int err = FFFS_OK;
    FFFS_PROFILE_PUSH(file->fs, FFFS_PROFILE_CLOSE);
    if ((file->flags & FFFS_O_WRONLY) != 0) {
        err = flush_write_cache(file, true);
        if (err == FFFS_OK) {
            if (file->current == file->span_head) {
                err = fffs_begin_extent_metadata(file, file->current,
                        file->current_sector_serial, file->data_offset,
                        file->current_metadata_offset,
                        file->current_needs_footer, file->current_data_len,
                        file->current_file_offset);
            } else {
                err = fffs_write_extent_metadata(file, file->current,
                        file->current_sector_serial, file->data_offset,
                        file->current_metadata_offset,
                        file->current_needs_footer, file->current_data_len,
                        1, 0, 0, file->current_file_offset, false);
            }
        }
        if (err == FFFS_OK) {
            bool root_deferred = file->current != file->head &&
                file->span_head != file->head;
            err = flush_pending_span_head(file, 0, true);
            if (err == FFFS_OK && root_deferred) {
                err = fffs_finish_deferred_root_metadata(file);
            }
            if (err == FFFS_OK) {
                fffs_fsinfo_note_committed_add(file->fs, file->size);
            }
            if (err == FFFS_OK && file->old_head != 0) {
                bool root_accounted = false;
                (void)fffs_invalidate_old_chain(file->fs, file->slot,
                        file->old_head, file->old_next, true,
                        &root_accounted);
                if (fffs_fsinfo_committed_valid(file->fs) &&
                        !root_accounted) {
                    fffs_fsinfo_invalidate_committed(file->fs);
                }
            }
        }
        fffs_alloc_release_reservation(file);
        unregister_inflight_writer(file);
    }
    file->closed = true;
    FFFS_PROFILE_POP(file->fs, FFFS_PROFILE_CLOSE);
    return err;
}
