/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS core API implementation: formatting, mounting, namespace lookup,
 * file streaming, stat/delete operations, and prefix directory iteration.
 */

#include "fffs_internal.h"

#include <stdbool.h>
#include <string.h>

#define FFFS_SECTORS_PER_FILE_Q8_ONE 256u

static int format_sector_shift(enum fffs_sector_size sector_size,
        uint8_t *sector_shift) {
    size_t size = sector_size == FFFS_SECTOR_DEFAULT ?
        FFFS_DEFAULT_SECTOR_SIZE : (size_t)sector_size;
    for (uint8_t shift = FFFS_MIN_SECTOR_SHIFT;
            shift <= FFFS_MAX_SECTOR_SHIFT; shift++) {
        if (((size_t)256u << shift) == size) {
            *sector_shift = shift;
            return FFFS_OK;
        }
    }
    return FFFS_ERR_INVALID;
}

static void fsinfo_invalidate_committed(struct fffs *fs) {
    fs->fsinfo_valid_flags &= ~(uint32_t)(
            FFFS_FSINFO_COMMITTED_FILES_VALID |
            FFFS_FSINFO_COMMITTED_BYTES_VALID |
            FFFS_FSINFO_METADATA_ESTIMATE_VALID);
}

static bool fsinfo_committed_valid(const struct fffs *fs) {
    return (fs->fsinfo_valid_flags &
            (FFFS_FSINFO_COMMITTED_FILES_VALID |
             FFFS_FSINFO_COMMITTED_BYTES_VALID)) ==
        (FFFS_FSINFO_COMMITTED_FILES_VALID |
         FFFS_FSINFO_COMMITTED_BYTES_VALID);
}

static void fsinfo_note_committed_add(struct fffs *fs, uint32_t size) {
    if (!fsinfo_committed_valid(fs)) {
        return;
    }
    fs->committed_file_count += 1;
    fs->committed_data_bytes += size;
    fs->fsinfo_valid_flags &= ~FFFS_FSINFO_METADATA_ESTIMATE_VALID;
}

void fffs_fsinfo_note_committed_delete(struct fffs *fs, uint32_t size) {
    if (!fsinfo_committed_valid(fs)) {
        return;
    }
    if (fs->committed_file_count == 0 || fs->committed_data_bytes < size) {
        fsinfo_invalidate_committed(fs);
        return;
    }
    fs->committed_file_count -= 1;
    fs->committed_data_bytes -= size;
    fs->fsinfo_valid_flags &= ~FFFS_FSINFO_METADATA_ESTIMATE_VALID;
}

static bool invalidate_old_chain(struct fffs *fs, uint16_t slot,
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
        if (!have_next) {
            int err = fffs_read_metadata_for_slot(fs, current, slot, NULL,
                    NULL, NULL, &current_next, NULL);
            if (err != FFFS_OK) {
                return false;
            }
        }
        fffs_alloc_map_mark_unknown(fs, current);
        visited += 1;
#if !FFFS_LAZY_DELETE_TOMBSTONES
        if (tombstone) {
            bool accounted = false;
            int err = fffs_tombstone_metadata_for_slot(fs, current, slot,
                    FFFS_TOMBSTONE_COMMITTED_DELETE,
                    depth == 0 ? &accounted : NULL);
            if (err != FFFS_OK) {
                return false;
            }
            if (depth == 0 && root_accounted) {
                *root_accounted = accounted;
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

static int assigned_sector_serial(struct fffs_file *file, uint32_t *serial) {
    if (file->current_needs_footer) {
        *serial = claim_sector_serial(file->fs);
        return FFFS_OK;
    }
    return fffs_read_sector_footer(file->fs, file->current, serial);
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

static size_t root_payload_offset_for_name(const char *name) {
    return 1u + strlen(name) + 1u;
}

static void stage_root_prefix(struct fffs_file *file, const char *name) {
    size_t name_len = strlen(name);
    file->root_payload_offset = (uint16_t)root_payload_offset_for_name(name);
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

bool fffs_sector_is_inflight(struct fffs *fs, uint16_t sector) {
    for (struct fffs_file *file = fs->inflight_writers; file;
            file = file->inflight_next) {
        if (file->closed || (file->flags & FFFS_O_WRONLY) == 0) {
            continue;
        }
        if (file->head == sector || file->current == sector) {
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

int fffs_format(const struct fffs_backend *backend,
        const struct fffs_format_options *options) {
    if (!fffs_valid_backend(backend)) {
        return FFFS_ERR_INVALID;
    }

    uint8_t index_sectors = options && options->index_sectors ?
        options->index_sectors : FFFS_DEFAULT_INDEX_SECTORS;
    uint8_t sector_shift = FFFS_DEFAULT_SECTOR_SHIFT;
    int err = format_sector_shift(options ? options->sector_size :
            FFFS_SECTOR_DEFAULT, &sector_shift);
    if (err != FFFS_OK) {
        return err;
    }
    size_t sector_size = (size_t)256u << sector_shift;
    if (index_sectors < 2 || index_sectors > 15 ||
            backend->size % sector_size != 0 ||
            backend->size / sector_size < index_sectors ||
            backend->size / sector_size > UINT16_MAX) {
        return FFFS_ERR_INVALID;
    }

    size_t index_area_size = sector_size * index_sectors;
    size_t erase_size = sector_size > 8192 ? sector_size : 8192;
    if (erase_size < index_area_size) {
        erase_size = index_area_size;
    }
    if (erase_size > backend->size) {
        erase_size = backend->size;
    }
    err = fffs_map_backend_status(backend->erase(backend->ctx, 0,
                erase_size));
    if (err != FFFS_OK) {
        return err;
    }

    return fffs_program_index_header(backend, 0, index_sectors, sector_shift,
            0);
}

int fffs_mount(struct fffs *fs, const struct fffs_backend *backend,
        const struct fffs_mount_options *options) {
    if (!fs || !fffs_valid_backend(backend) || !options ||
            !fffs_index_cache_config_valid(options->index_hash_table_size)) {
        return FFFS_ERR_INVALID;
    }
#if FFFS_INDEX_CACHE_MODE != FFFS_INDEX_CACHE_NONE
    if (!options->index_cache || options->index_cache_size <
            fffs_index_cache_required_size(options->index_hash_table_size)) {
        return FFFS_ERR_INVALID;
    }
#endif
    void *index_cache = options->index_cache;
    uint16_t *index_heads = NULL;
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
    index_heads = index_cache;
#endif
    size_t index_hash_table_size = options->index_hash_table_size;
    if (!options->scratch || options->scratch_size < FFFS_MIN_SCRATCH_SIZE ||
            options->scratch_size < backend->read_granule) {
        return FFFS_ERR_INVALID;
    }
    *fs = (struct fffs){0};
#if FFFS_INDEX_CACHE_MODE != FFFS_INDEX_CACHE_NONE
    memset(index_cache, 0, fffs_index_cache_required_size(
                index_hash_table_size));
#endif

    struct fffs_index_sequence sequence;
    int err = fffs_find_index_sequence(backend, &sequence);
    if (err != FFFS_OK) {
        return err;
    }
    size_t sector_size = (size_t)256u << sequence.sector_shift;
    if (backend->size % sector_size != 0 ||
            backend->size / sector_size > UINT16_MAX) {
        return FFFS_ERR_CORRUPT;
    }

    fs->backend = *backend;
    fs->index_cache = index_cache;
    fs->index_heads = index_heads;
    fs->index_hash_table_size = index_hash_table_size;
#if FFFS_PROFILE_TRACE
    fs->profile_trace = options->profile_trace;
    fs->profile_trace_user = options->profile_trace_user;
#endif
    fs->scratch = options->scratch;
    fs->scratch_size = options->scratch_size;
    fs->sector_size = sector_size;
    fs->sector_count = backend->size / fs->sector_size;
    fs->sector_shift = sequence.sector_shift;
    fs->index_sectors = sequence.index_sectors;
    fs->oldest_index_sector = sequence.oldest_sector;
    fs->index_sequence_count = sequence.count;
    fs->active_index_sector = sequence.active_sector;
    fs->active_index_serial = sequence.active_serial;
    fs->next_index_offset = sequence.active_sector * fs->sector_size +
        FFFS_HEADER_SIZE;
    fs->alloc_cursor = fs->index_sectors;
    fs->gc_cursor = fs->index_sectors;
    fs->next_sector_serial = 1;
    fs->avg_sectors_per_file_q8 = FFFS_SECTORS_PER_FILE_Q8_ONE;

    err = fffs_index_finish_interrupted_compaction(fs);
    if (err != FFFS_OK) {
        fffs_unmount(fs);
        return err;
    }

    err = fffs_alloc_map_mount_init(fs, options);
    if (err != FFFS_OK) {
        fffs_unmount(fs);
        return err;
    }

    err = fffs_replay_index(fs);
    if (err != FFFS_OK) {
        fffs_unmount(fs);
        return err;
    }
    return FFFS_OK;
}

void fffs_unmount(struct fffs *fs) {
    if (!fs) {
        return;
    }
    *fs = (struct fffs){0};
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
    file->found = found;

    if (read) {
        file->cache_len = read_cache.len;
        file->cache_data_pos = read_cache.data_pos;
        file->data_offset = resolved_payload_data_off;
        file->size = resolved_st.size;
        file->current = head;
        file->current_data_len = resolved_payload_data_len;
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
        err = assigned_sector_serial(file, &file->current_sector_serial);
        if (err != FFFS_OK) {
            return err;
        }
        file->root_sector_serial = file->current_sector_serial;
        memcpy(file->name, name, strlen(name) + 1);
        stage_root_prefix(file, name);
        register_inflight_writer(file);
    }

    return FFFS_OK;
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
            file->fs->sector_size + file->data_offset + file->current_data_pos,
            file->cache, n);
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
            if (file->current_next == 0) {
                FFFS_PROFILE_POP(file->fs, FFFS_PROFILE_READ);
                return FFFS_ERR_CORRUPT;
            }
            uint16_t data_len;
            uint16_t next;
            int err = fffs_read_metadata_for_slot(file->fs,
                    file->current_next, file->slot, NULL,
                    &file->data_offset, &data_len, &next, NULL);
            if (err != FFFS_OK) {
                FFFS_PROFILE_POP(file->fs, FFFS_PROFILE_READ);
                return err;
            }
            file->current = file->current_next;
            file->current_data_len = data_len;
            file->current_next = next;
            file->current_data_pos = 0;
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

static int start_next_extent(struct fffs_file *file) {
    int err = flush_write_cache(file, true);
    if (err != FFFS_OK) {
        return err;
    }

    uint16_t old_sector = file->current;
    uint16_t old_data_off = file->data_offset;
    uint16_t old_record_off = file->current_metadata_offset;
    bool old_needs_footer = file->current_needs_footer;
    uint16_t old_data_len = file->current_data_len;
    uint32_t old_serial = file->current_sector_serial;
    uint32_t old_file_offset = file->current_file_offset;
    bool old_is_root = file->current == file->head;

    uint16_t next_sector;
    err = fffs_alloc_next_sector(file, &next_sector);
    if (err != FFFS_OK) {
        return err;
    }

    if (old_is_root) {
        file->root_data_len = old_data_len;
        file->root_data_offset = old_data_off;
        file->root_metadata_offset = old_record_off;
        file->root_needs_footer = old_needs_footer;
        file->root_next = next_sector;
        file->root_sector_serial = old_serial;
        file->root_deferred = true;
        file->current_file_offset = file->root_data_len;
    } else {
        err = fffs_write_extent_metadata(file, old_sector, old_serial,
                old_data_off, old_record_off, old_needs_footer,
                old_data_len, 0, next_sector, old_file_offset, false);
        if (err != FFFS_OK) {
            return err;
        }
        file->current_file_offset = old_file_offset + old_data_len;
    }

    file->current = next_sector;
    err = assigned_sector_serial(file, &file->current_sector_serial);
    if (err != FFFS_OK) {
        return err;
    }
    file->current_data_len = 0;
    file->current_next = 0;
    file->cache_len = 0;
    return FFFS_OK;
}

int fffs_write(struct fffs_file *file, const void *buffer, size_t size,
        size_t *out_written) {
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
    if (out_written) {
        *out_written = size;
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
    return fffs_read_metadata_for_slot(file->fs, file->head, file->slot, st,
            NULL, NULL, NULL, NULL);
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
            if (file->current != file->head) {
                err = fffs_write_extent_metadata(file, file->current,
                        file->current_sector_serial, file->data_offset,
                        file->current_metadata_offset,
                        file->current_needs_footer, file->current_data_len,
                        0, 0, file->current_file_offset, false);
            }
        }
        if (err == FFFS_OK) {
            uint16_t root_len = file->root_deferred ?
                file->root_data_len : file->current_data_len;
            uint16_t root_next = file->root_deferred ? file->root_next : 0;
            uint32_t root_serial = file->root_deferred ?
                file->root_sector_serial : file->current_sector_serial;
            uint16_t root_data_off = file->root_deferred ?
                file->root_data_offset : file->data_offset;
            uint16_t root_record_off = file->root_deferred ?
                file->root_metadata_offset : file->current_metadata_offset;
            bool root_needs_footer = file->root_deferred ?
                file->root_needs_footer : file->current_needs_footer;
            err = fffs_write_extent_metadata(file, file->head, root_serial,
                    root_data_off, root_record_off, root_needs_footer,
                    root_len, file->size, root_next, 0, true);
            if (err == FFFS_OK) {
                fsinfo_note_committed_add(file->fs, file->size);
            }
            if (err == FFFS_OK && file->old_head != 0) {
                bool root_accounted = false;
                (void)invalidate_old_chain(file->fs, file->slot,
                        file->old_head, file->old_next, true,
                        &root_accounted);
                if (fsinfo_committed_valid(file->fs) && !root_accounted) {
                    fsinfo_invalidate_committed(file->fs);
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

int fffs_stat(struct fffs *fs, const char *name, struct fffs_stat *st) {
    if (!fs || !name || !st) {
        return FFFS_ERR_INVALID;
    }
    uint16_t slot;
    uint16_t head;
    bool found;
    FFFS_PROFILE_PUSH(fs, FFFS_PROFILE_INDEX_RESOLVE);
    int err = fffs_index_resolve(fs, name, &slot, &head, &found, st, NULL,
            NULL, NULL, NULL);
    FFFS_PROFILE_POP(fs, FFFS_PROFILE_INDEX_RESOLVE);
    if (err != FFFS_OK) {
        return err;
    }
    if (!found) {
        return FFFS_ERR_NOT_FOUND;
    }
    return FFFS_OK;
}

int fffs_exists(struct fffs *fs, const char *name, bool *exists) {
    if (!fs || !name || !exists) {
        return FFFS_ERR_INVALID;
    }
    struct fffs_stat st;
    int err = fffs_stat(fs, name, &st);
    if (err == FFFS_OK) {
        *exists = true;
        return FFFS_OK;
    }
    if (err == FFFS_ERR_NOT_FOUND) {
        *exists = false;
        return FFFS_OK;
    }
    return err;
}

int fffs_delete_file(struct fffs *fs, const char *name) {
    if (!fs || !name) {
        return FFFS_ERR_INVALID;
    }
    uint16_t slot;
    uint16_t head;
    uint16_t next = 0;
    bool found;
    FFFS_PROFILE_PUSH(fs, FFFS_PROFILE_DELETE);
    FFFS_PROFILE_PUSH(fs, FFFS_PROFILE_INDEX_RESOLVE);
    int err = fffs_index_resolve(fs, name, &slot, &head, &found, NULL, NULL,
            NULL, &next, NULL);
    FFFS_PROFILE_POP(fs, FFFS_PROFILE_INDEX_RESOLVE);
    if (err != FFFS_OK) {
        FFFS_PROFILE_POP(fs, FFFS_PROFILE_DELETE);
        return err;
    }
    if (!found) {
        FFFS_PROFILE_POP(fs, FFFS_PROFILE_DELETE);
        return FFFS_ERR_NOT_FOUND;
    }
    err = fffs_append_index_record(fs, slot, 0);
    if (err != FFFS_OK) {
        FFFS_PROFILE_POP(fs, FFFS_PROFILE_DELETE);
        return err;
    }
    bool root_accounted = false;
    (void)invalidate_old_chain(fs, slot, head, next, true, &root_accounted);
    if (fsinfo_committed_valid(fs) && !root_accounted) {
        fsinfo_invalidate_committed(fs);
    }
    FFFS_PROFILE_POP(fs, FFFS_PROFILE_DELETE);
    return FFFS_OK;
}

int fffs_fsinfo(struct fffs *fs, struct fffs_fsinfo *info, uint32_t flags) {
    const uint32_t known_flags = FFFS_FSINFO_REFRESH_COMMITTED |
        FFFS_FSINFO_ESTIMATE_METADATA | FFFS_FSINFO_REFRESH_IF_NEEDED;
    if (!fs || !info || (flags & ~known_flags) != 0) {
        return FFFS_ERR_INVALID;
    }

    bool refresh_committed =
        (flags & FFFS_FSINFO_REFRESH_COMMITTED) != 0 ||
        ((flags & FFFS_FSINFO_REFRESH_IF_NEEDED) != 0 &&
         !fsinfo_committed_valid(fs));
    if (refresh_committed) {
        uint32_t file_count = 0;
        uint32_t byte_count = 0;
        struct fffs_index_iter iter = {
            .fs = fs,
        };
        uint16_t slot;
        uint16_t head;
        while (fffs_index_iter_read(&iter, &slot, &head)) {
            uint32_t size;
            int err = fffs_read_root_size_for_slot(fs, head, slot, &size);
            if (err != FFFS_OK) {
                fsinfo_invalidate_committed(fs);
                return err;
            }
            file_count += 1;
            byte_count += size;
        }
        if (iter.status != FFFS_OK) {
            fsinfo_invalidate_committed(fs);
            return iter.status;
        }

        fs->committed_file_count = file_count;
        fs->committed_data_bytes = byte_count;
        fs->fsinfo_valid_flags |= FFFS_FSINFO_COMMITTED_FILES_VALID |
            FFFS_FSINFO_COMMITTED_BYTES_VALID;
        fs->fsinfo_valid_flags &= ~FFFS_FSINFO_METADATA_ESTIMATE_VALID;
    }

    *info = (struct fffs_fsinfo){0};
    size_t data_sectors = fs->sector_count > fs->index_sectors ?
        fs->sector_count - fs->index_sectors : 0;
    info->total_bytes = (uint32_t)(data_sectors *
            (fs->sector_size - FFFS_SECTOR_FOOTER_SIZE));
    info->valid_flags |= FFFS_FSINFO_TOTAL_VALID;

    for (struct fffs_file *file = fs->inflight_writers; file;
            file = file->inflight_next) {
        info->inflight_file_count += 1;
        info->inflight_data_bytes += file->size;
    }
    info->valid_flags |= FFFS_FSINFO_INFLIGHT_VALID;

    if (fsinfo_committed_valid(fs)) {
        info->committed_file_count = fs->committed_file_count;
        info->committed_data_bytes = fs->committed_data_bytes;
        info->valid_flags |= FFFS_FSINFO_COMMITTED_FILES_VALID |
            FFFS_FSINFO_COMMITTED_BYTES_VALID;
    }

    if ((flags & FFFS_FSINFO_ESTIMATE_METADATA) != 0 &&
            fsinfo_committed_valid(fs)) {
        uint32_t estimated = (fs->committed_file_count *
                (uint32_t)FFFS_MD_FILE_RECORD_SIZE *
                (uint32_t)fs->avg_sectors_per_file_q8) /
            FFFS_SECTORS_PER_FILE_Q8_ONE;
        info->estimated_metadata_bytes = estimated;
        info->valid_flags |= FFFS_FSINFO_METADATA_ESTIMATE_VALID;
    }

    return FFFS_OK;
}

int fffs_dir_open(struct fffs *fs, struct fffs_dir *dir,
        const char *prefix) {
    if (!fs || !dir) {
        return FFFS_ERR_INVALID;
    }
    *dir = (struct fffs_dir){0};
    dir->fs = fs;
    if (prefix) {
        size_t prefix_len = strlen(prefix);
        if (prefix_len > FFFS_MAX_NAME) {
            dir->status = FFFS_ERR_NAME_TOO_LONG;
            dir->closed = true;
            return FFFS_ERR_NAME_TOO_LONG;
        }
        memcpy(dir->prefix, prefix, prefix_len + 1);
        dir->prefix_len = prefix_len;
    }
    return FFFS_OK;
}

bool fffs_dir_read(struct fffs_dir *dir, struct fffs_stat *st) {
    if (!dir || dir->closed || !st) {
        if (dir) {
            dir->status = FFFS_ERR_INVALID;
        }
        return false;
    }
    FFFS_PROFILE_PUSH(dir->fs, FFFS_PROFILE_DIR_READ);
    bool found = fffs_index_dir_read(dir, st);
    FFFS_PROFILE_POP(dir->fs, FFFS_PROFILE_DIR_READ);
    return found;
}

int fffs_dir_status(const struct fffs_dir *dir) {
    if (!dir) {
        return FFFS_ERR_INVALID;
    }
    return dir->status;
}

int fffs_dir_close(struct fffs_dir *dir) {
    if (!dir || dir->closed) {
        return FFFS_ERR_INVALID;
    }
    dir->closed = true;
    return FFFS_OK;
}

int fffs_list(struct fffs *fs, struct fffs_stat *entries,
        size_t capacity, size_t *out_count) {
    if (!fs || (!entries && capacity)) {
        return FFFS_ERR_INVALID;
    }

    struct fffs_dir dir;
    int err = fffs_dir_open(fs, &dir, NULL);
    if (err != FFFS_OK) {
        return err;
    }

    struct fffs_stat st;
    size_t count = 0;
    while (fffs_dir_read(&dir, &st)) {
        if (count < capacity) {
            entries[count] = st;
        }
        count += 1;
    }
    err = fffs_dir_status(&dir);
    if (err != FFFS_OK) {
        return err;
    }
    if (out_count) {
        *out_count = count;
    }
    return fffs_dir_close(&dir);
}

const char *fffs_status_name(int status) {
    switch (status) {
    case FFFS_OK:
        return "ok";
    case FFFS_ERR_INVALID:
        return "invalid";
    case FFFS_ERR_NOMEM:
        return "nomem";
    case FFFS_ERR_RANGE:
        return "range";
    case FFFS_ERR_NO_SPACE:
        return "no_space";
    case FFFS_ERR_NOT_FOUND:
        return "not_found";
    case FFFS_ERR_EXISTS:
        return "exists";
    case FFFS_ERR_NAME_TOO_LONG:
        return "name_too_long";
    case FFFS_ERR_CORRUPT:
        return "corrupt";
    case FFFS_ERR_IO:
        return "io";
    default:
        return "unknown";
    }
}
