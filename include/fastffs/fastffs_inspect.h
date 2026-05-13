/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS host inspection API: image dump/check summaries and deterministic
 * workload generation for verification and recovery tooling.
 */

#ifndef FASTFFS_FASTFFS_INSPECT_H
#define FASTFFS_FASTFFS_INSPECT_H

#include "fastffs/fastffs.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fffs_inspect_summary {
    size_t sector_size;
    size_t sector_count;
    uint8_t index_sectors;
    size_t active_index_sector;
    uint8_t active_index_serial;

    size_t index_records;
    size_t index_deletes;
    size_t index_corrupt_records;
    size_t live_entries;
    size_t live_entries_corrupt;

    size_t data_sectors_erased;
    size_t data_sectors_owned;
    size_t data_sectors_tombstoned;
    size_t data_sectors_corrupt;
    size_t md_live;
    size_t md_obsolete_orphaned;
    size_t md_tombstoned;
    size_t md_corrupt;
};

struct fffs_workload_options {
    uint32_t seed;
    size_t rounds;
    size_t file_count;
    size_t max_file_size;
};

struct fffs_workload_summary {
    size_t writes;
    size_t deletes;
    size_t reads;
    size_t lists;
};

int fffs_inspect_check(const struct fffs_backend *backend,
        struct fffs_inspect_summary *summary);
int fffs_inspect_dump(const struct fffs_backend *backend, FILE *out);
int fffs_workload_run(struct fffs *fs,
        const struct fffs_workload_options *options,
        struct fffs_workload_summary *summary);

#ifdef __cplusplus
}
#endif

#endif
