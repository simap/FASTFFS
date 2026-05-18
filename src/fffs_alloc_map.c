/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS allocation map hints: optional RAM state used by allocation and GC to
 * skip sectors already known to be unavailable without becoming namespace
 * authority.
 */

#include "fffs_internal.h"

#include <string.h>

#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
static size_t map_words_for_sectors(size_t sector_count) {
    return (sector_count + 31u) / 32u;
}

static uint32_t map_mask(uint16_t sector) {
    return UINT32_C(1) << (sector & 31u);
}
#endif

bool fffs_alloc_map_config_valid(size_t sector_count,
        const struct fffs_mount_options *opts) {
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    return opts && opts->alloc_map &&
        opts->alloc_map_words >= map_words_for_sectors(sector_count);
#else
    (void)sector_count;
    (void)opts;
    return true;
#endif
}

int fffs_alloc_map_mount_init(struct fffs *fs,
        const struct fffs_mount_options *opts) {
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    size_t words = map_words_for_sectors(fs->sector_count);
    if (!fffs_alloc_map_config_valid(fs->sector_count, opts)) {
        return FFFS_ERR_INVALID;
    }
    fs->alloc_map = opts->alloc_map;
    fs->alloc_map_words = opts->alloc_map_words;
    memset(fs->alloc_map, 0, words * sizeof(fs->alloc_map[0]));
    for (uint16_t sector = 0; sector < fs->index_sectors; sector++) {
        fffs_alloc_map_mark_used(fs, sector);
    }
    return FFFS_OK;
#else
    (void)fs;
    (void)opts;
    return FFFS_OK;
#endif
}

bool fffs_alloc_map_maybe_used(struct fffs *fs, uint16_t sector) {
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    if (!fs->alloc_map || sector >= fs->sector_count) {
        return false;
    }
    return (fs->alloc_map[sector / 32u] & map_mask(sector)) != 0;
#else
    (void)fs;
    (void)sector;
    return false;
#endif
}

void fffs_alloc_map_mark_used(struct fffs *fs, uint16_t sector) {
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    if (!fs->alloc_map || sector >= fs->sector_count) {
        return;
    }
    fs->alloc_map[sector / 32u] |= map_mask(sector);
#else
    (void)fs;
    (void)sector;
#endif
}

void fffs_alloc_map_mark_unknown(struct fffs *fs, uint16_t sector) {
#if FFFS_ALLOC_MAP_MODE == FFFS_ALLOC_MAP_FULL_BITMAP
    if (!fs->alloc_map || sector >= fs->sector_count) {
        return;
    }
    fs->alloc_map[sector / 32u] &= ~map_mask(sector);
#else
    (void)fs;
    (void)sector;
#endif
}

void fffs_alloc_map_mark_range_unknown(struct fffs *fs,
        uint16_t first, uint16_t count) {
    for (uint16_t i = 0; i < count; i++) {
        fffs_alloc_map_mark_unknown(fs, (uint16_t)(first + i));
    }
}
