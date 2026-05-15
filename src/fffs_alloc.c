/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS core allocation policy: erased-space probing, allocation cursor
 * traversal, live-head checks, and allocation-pressure GC fallback.
 */

#include "fffs_internal.h"

int fffs_flash_span_is_erased(struct fffs *fs, size_t offset, size_t size) {
    uint8_t *chunk = fs->scratch;
    size_t chunk_size = fs->scratch_size;
    while (size > 0) {
        size_t n = size < chunk_size ? size : chunk_size;
        int err = fffs_flash_read(fs, offset, chunk, n);
        if (err != FFFS_OK) {
            return err;
        }
        fffs_scratch_bump(fs);
        for (size_t i = 0; i < n; i++) {
            if (chunk[i] != 0xff) {
                return FFFS_ERR_NO_SPACE;
            }
        }
        offset += n;
        size -= n;
    }
    return FFFS_OK;
}

size_t fffs_next_data_sector(struct fffs *fs, size_t sector) {
    sector++;
    if (sector < fs->index_sectors || sector >= fs->sector_count) {
        return fs->index_sectors;
    }
    return sector;
}

static bool sector_in_reservation(const struct fffs_file *file,
        uint16_t sector) {
    if (!file || file->reserve_count == 0) {
        return false;
    }
    return sector >= file->reserve_first &&
        sector < (uint16_t)(file->reserve_first + file->reserve_count);
}

static bool sector_reserved_by_other(struct fffs_file *owner,
        uint16_t sector) {
    struct fffs *fs = owner->fs;
    for (struct fffs_file *file = fs->inflight_writers; file;
            file = file->inflight_next) {
        if (file == owner || file->closed ||
                (file->flags & FFFS_O_WRONLY) == 0) {
            continue;
        }
        if (sector_in_reservation(file, sector)) {
            return true;
        }
    }
    return false;
}

void fffs_alloc_release_reservation(struct fffs_file *file) {
    if (!file || !file->fs || file->reserve_count == 0) {
        return;
    }
    for (uint16_t i = 0; i < file->reserve_count; i++) {
        fffs_alloc_map_mark_unknown(file->fs,
                (uint16_t)(file->reserve_first + i));
    }
    file->reserve_first = 0;
    file->reserve_count = 0;
}

#if FFFS_ALLOC_RESERVE_SECTORS > 0
static bool sector_can_be_reserved(struct fffs_file *file, uint16_t sector) {
    struct fffs *fs = file->fs;
    if (sector < fs->index_sectors || sector >= fs->sector_count) {
        return false;
    }
    if (fffs_alloc_map_maybe_used(fs, sector)) {
        return false;
    }
    if (fffs_sector_is_inflight(fs, sector)) {
        return false;
    }
    return !sector_reserved_by_other(file, sector);
}
#endif

static bool halve_reservations(struct fffs *fs) {
    bool releasedAny = false;
    for (struct fffs_file *file = fs->inflight_writers; file;
            file = file->inflight_next) {
        if (file->closed || (file->flags & FFFS_O_WRONLY) == 0 ||
                file->reserve_count == 0) {
            continue;
        }
        uint16_t keep_count = (uint16_t)(file->reserve_count >> 1u);
        uint16_t release_first = (uint16_t)(file->reserve_first + keep_count);
        uint16_t release_count =
            (uint16_t)(file->reserve_count - keep_count);
        for (uint16_t i = 0; i < release_count; i++) {
            fffs_alloc_map_mark_unknown(fs, (uint16_t)(release_first + i));
        }
        file->reserve_count = keep_count;
        if (file->reserve_count == 0) {
            file->reserve_first = 0;
        }
        releasedAny = true;
    }
    return releasedAny;
}

static void reserve_after(struct fffs_file *file, uint16_t sector) {
#if FFFS_ALLOC_RESERVE_SECTORS <= 0
    (void)file;
    (void)sector;
#else
    struct fffs *fs = file->fs;
    uint16_t next = (uint16_t)(sector + 1u);
    if (file->reserve_count != 0) {
        next = (uint16_t)(file->reserve_first + file->reserve_count);
    }
    while (file->reserve_count < FFFS_ALLOC_RESERVE_SECTORS &&
            next < fs->sector_count) {
        if (!sector_can_be_reserved(file, next)) {
            break;
        }
        if (file->reserve_count == 0) {
            file->reserve_first = next;
        }
        file->reserve_count++;
        fffs_alloc_map_mark_used(fs, next);
        next++;
    }
#endif
}

static int try_reserved_sector(struct fffs_file *file, uint16_t *sector,
        uint16_t *skip_sector, bool *have_skip) {
    if (file->reserve_count == 0) {
        return FFFS_ERR_NO_SPACE;
    }
    uint16_t candidate = file->reserve_first;
    int err = fffs_flash_span_is_erased(file->fs,
            (size_t)candidate * file->fs->sector_size,
            file->fs->sector_size);
    if (err == FFFS_OK) {
        *sector = candidate;
        file->reserve_first++;
        file->reserve_count--;
        if (file->reserve_count == 0) {
            file->reserve_first = 0;
        }
        fffs_alloc_map_mark_used(file->fs, candidate);
        reserve_after(file, candidate);
        return FFFS_OK;
    }
    if (err != FFFS_ERR_NO_SPACE) {
        return err;
    }
    fffs_alloc_release_reservation(file);
    *skip_sector = candidate;
    *have_skip = true;
    return FFFS_ERR_NO_SPACE;
}

static int alloc_erased_sector_once(struct fffs_file *file, uint16_t *sector,
        bool use_map, uint16_t skip_sector, bool have_skip) {
    struct fffs *fs = file->fs;
    if (fs->sector_count <= fs->index_sectors) {
        return FFFS_ERR_NO_SPACE;
    }

    size_t s = fs->alloc_cursor;
    if (s < fs->index_sectors || s >= fs->sector_count) {
        s = fs->index_sectors;
    }

    size_t data_sectors = fs->sector_count - fs->index_sectors;
    for (size_t checked = 0; checked < data_sectors; checked++) {
        if (have_skip && s == skip_sector) {
            s = fffs_next_data_sector(fs, s);
            continue;
        }
        if (use_map && fffs_alloc_map_maybe_used(fs, (uint16_t)s)) {
            s = fffs_next_data_sector(fs, s);
            continue;
        }
        if (fffs_sector_is_inflight(fs, (uint16_t)s) ||
                sector_reserved_by_other(file, (uint16_t)s)) {
            fffs_alloc_map_mark_used(fs, (uint16_t)s);
            s = fffs_next_data_sector(fs, s);
            continue;
        }
        int err = fffs_flash_span_is_erased(fs, s * fs->sector_size,
                fs->sector_size);
        if (err == FFFS_OK) {
            *sector = (uint16_t)s;
            fs->alloc_cursor = fffs_next_data_sector(fs, s);
            fffs_alloc_map_mark_used(fs, *sector);
            reserve_after(file, *sector);
            return FFFS_OK;
        }
        if (err != FFFS_ERR_NO_SPACE) {
            return err;
        }
        s = fffs_next_data_sector(fs, s);
    }
    return FFFS_ERR_NO_SPACE;
}

#if FFFS_GC_ON_ALLOC_FAILURE
static int allocate_erased_gc_sector(struct fffs_file *file,
        uint16_t *sector) {
    struct fffs *fs = file->fs;
    uint16_t erased_sector;
    int err = fffs_gc_until_erased(fs, &erased_sector);
    if (err != FFFS_OK) {
        return err;
    }
    if (fffs_sector_is_inflight(fs, erased_sector) ||
            sector_reserved_by_other(file, erased_sector)) {
        return FFFS_ERR_CORRUPT;
    }
    err = fffs_flash_span_is_erased(fs,
            (size_t)erased_sector * fs->sector_size, fs->sector_size);
    if (err != FFFS_OK) {
        return err;
    }
    *sector = erased_sector;
    fs->alloc_cursor = fffs_next_data_sector(fs, erased_sector);
    fffs_alloc_map_mark_used(fs, erased_sector);
    reserve_after(file, erased_sector);
    return FFFS_OK;
}
#endif

int fffs_alloc_next_sector(struct fffs_file *file, uint16_t *sector) {
    if (!file || !file->fs || !sector) {
        return FFFS_ERR_INVALID;
    }
    struct fffs *fs = file->fs;
    uint16_t skip_sector = 0;
    bool have_skip = false;
    int err = try_reserved_sector(file, sector, &skip_sector, &have_skip);
    if (err == FFFS_OK) {
        return FFFS_OK;
    }
    if (err != FFFS_ERR_NO_SPACE) {
        return err;
    }
    err = alloc_erased_sector_once(file, sector, true, skip_sector,
            have_skip);
    if (err == FFFS_ERR_NO_SPACE) {
        if (halve_reservations(fs)) {
            err = alloc_erased_sector_once(file, sector, true,
                    skip_sector, have_skip);
        }
    }
#if FFFS_GC_ON_ALLOC_FAILURE
    if (err == FFFS_ERR_NO_SPACE && fs->sector_count > fs->index_sectors) {
        return allocate_erased_gc_sector(file, sector);
    }
#endif
    return err;
}
