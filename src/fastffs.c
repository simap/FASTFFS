/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS core API implementation: formatting, mounting, namespace lookup,
 * stat/delete operations, fsinfo, and prefix directory iteration.
 */

#include "fffs_internal.h"

#include <stdbool.h>
#include <string.h>

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

void fffs_fsinfo_invalidate_committed(struct fffs *fs) {
    fs->fsinfo_valid_flags &= ~(uint32_t)(
            FFFS_FSINFO_COMMITTED_FILES_VALID |
            FFFS_FSINFO_COMMITTED_BYTES_VALID |
            FFFS_FSINFO_METADATA_ESTIMATE_VALID);
}

bool fffs_fsinfo_committed_valid(const struct fffs *fs) {
    return (fs->fsinfo_valid_flags &
            (FFFS_FSINFO_COMMITTED_FILES_VALID |
             FFFS_FSINFO_COMMITTED_BYTES_VALID)) ==
        (FFFS_FSINFO_COMMITTED_FILES_VALID |
         FFFS_FSINFO_COMMITTED_BYTES_VALID);
}

void fffs_fsinfo_note_committed_add(struct fffs *fs, uint32_t size) {
    if (!fffs_fsinfo_committed_valid(fs)) {
        return;
    }
    fs->committed_file_count += 1;
    fs->committed_data_bytes += size;
    fs->fsinfo_valid_flags &= ~FFFS_FSINFO_METADATA_ESTIMATE_VALID;
}

void fffs_fsinfo_note_committed_delete(struct fffs *fs, uint32_t size) {
    if (!fffs_fsinfo_committed_valid(fs)) {
        return;
    }
    if (fs->committed_file_count == 0 || fs->committed_data_bytes < size) {
        fffs_fsinfo_invalidate_committed(fs);
        return;
    }
    fs->committed_file_count -= 1;
    fs->committed_data_bytes -= size;
    fs->fsinfo_valid_flags &= ~FFFS_FSINFO_METADATA_ESTIMATE_VALID;
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
    if (!options->scratch || options->scratch_size < FFFS_MIN_SCRATCH_SIZE) {
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
    fs->strict = options->strict;
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
    fs->compaction_reserve_count = 0;
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
    (void)fffs_invalidate_old_chain(fs, slot, head, next, true,
            &root_accounted);
    if (fffs_fsinfo_committed_valid(fs) && !root_accounted) {
        fffs_fsinfo_invalidate_committed(fs);
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
         !fffs_fsinfo_committed_valid(fs));
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
            int err = fffs_read_file_root_md(fs, head, slot, NULL,
                    NULL, NULL, NULL, NULL, &size, NULL);
            if (err != FFFS_OK) {
                fffs_fsinfo_invalidate_committed(fs);
                return err;
            }
            file_count += 1;
            byte_count += size;
        }
        if (iter.status != FFFS_OK) {
            fffs_fsinfo_invalidate_committed(fs);
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

    if (fffs_fsinfo_committed_valid(fs)) {
        info->committed_file_count = fs->committed_file_count;
        info->committed_data_bytes = fs->committed_data_bytes;
        info->valid_flags |= FFFS_FSINFO_COMMITTED_FILES_VALID |
            FFFS_FSINFO_COMMITTED_BYTES_VALID;
    }

    if ((flags & FFFS_FSINFO_ESTIMATE_METADATA) != 0 &&
            fffs_fsinfo_committed_valid(fs)) {
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
