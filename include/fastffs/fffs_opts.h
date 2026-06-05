/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS compile-time core configuration: index cache modes, cache sizing,
 * and small recovery-policy constants selected by the application build.
 */

#ifndef FASTFFS_FFFS_OPTS_H
#define FASTFFS_FFFS_OPTS_H

/*
 * Index cache modes:
 *
 * FFFS_INDEX_CACHE_NONE keeps no persistent RAM namespace cache. Lookup,
 * directory iteration, GC liveness, and index compaction scan the on-flash
 * index as needed.
 *
 * FFFS_INDEX_CACHE_HASH_HEADS stores resolved slot plus head-sector entries in
 * a caller-provided power-of-two hash table. It uses twice the RAM of a
 * head-only table with the same bucket count, but bucket mechanics do not need
 * root metadata reads.
 *
 * FFFS_INDEX_CACHE_FULL_SLOT_HEADS stores one head sector per possible
 * resolved slot. It uses 128 KiB but provides direct slot lookup.
 */
#define FFFS_INDEX_CACHE_NONE 0
#define FFFS_INDEX_CACHE_HASH_HEADS 1
#define FFFS_INDEX_CACHE_FULL_SLOT_HEADS 2

#define FFFS_ALLOC_MAP_NONE 0
#define FFFS_ALLOC_MAP_FULL_BITMAP 1

#ifndef FFFS_INDEX_CACHE_MODE
#define FFFS_INDEX_CACHE_MODE FFFS_INDEX_CACHE_HASH_HEADS
#endif

#ifndef FFFS_ALLOC_MAP_MODE
#define FFFS_ALLOC_MAP_MODE FFFS_ALLOC_MAP_FULL_BITMAP
#endif

#ifndef FFFS_INDEX_HASH_TABLE_SIZE
#define FFFS_INDEX_HASH_TABLE_SIZE 1024
#endif

#ifndef FFFS_INDEX_HASH_TABLE_SIZE_MAX
#define FFFS_INDEX_HASH_TABLE_SIZE_MAX 16384
#endif

#ifndef FFFS_ALLOC_RECOVERY_LOOKAHEAD
#define FFFS_ALLOC_RECOVERY_LOOKAHEAD 4
#endif

#ifndef FFFS_ALLOC_RESERVE_SECTORS
#define FFFS_ALLOC_RESERVE_SECTORS 4
#endif

#ifndef FFFS_ALLOC_MIN_USABLE_FREE
#define FFFS_ALLOC_MIN_USABLE_FREE 128
#endif

#ifndef FFFS_ALLOC_MAX_MD_RECORDS_PER_SECTOR
#define FFFS_ALLOC_MAX_MD_RECORDS_PER_SECTOR 32
#endif

#ifndef FFFS_ALLOC_TARGET_DENSITY
#define FFFS_ALLOC_TARGET_DENSITY 6
#endif

#ifndef FFFS_GC_ON_ALLOC_FAILURE
#define FFFS_GC_ON_ALLOC_FAILURE 1
#endif

#ifndef FFFS_GC_PARANOID_REACHABILITY
#define FFFS_GC_PARANOID_REACHABILITY 0
#endif

#ifndef FFFS_COMPACTION_CANDIDATE_COUNT
#define FFFS_COMPACTION_CANDIDATE_COUNT 4
#endif
#if FFFS_COMPACTION_CANDIDATE_COUNT < 1
#error "FFFS_COMPACTION_CANDIDATE_COUNT must be at least 1"
#endif

#ifndef FFFS_COMPACTION_RESERVE_SECTORS
#define FFFS_COMPACTION_RESERVE_SECTORS 5
#endif

#ifndef FFFS_COMPACTION_RESERVE_BEST_FIT
#define FFFS_COMPACTION_RESERVE_BEST_FIT 0
#endif

#ifndef FFFS_INDEX_COMPACT_OUTER_SCAN_SIZE
#define FFFS_INDEX_COMPACT_OUTER_SCAN_SIZE 64
#endif

#ifndef FFFS_INDEX_COMPACT_WRITE_BUFFER_SIZE
#define FFFS_INDEX_COMPACT_WRITE_BUFFER_SIZE 64
#endif

#ifndef FFFS_MD_PRELOAD_MAX
#define FFFS_MD_PRELOAD_MAX 128
#endif

#ifndef FFFS_MIN_SCRATCH_SIZE
#define FFFS_MIN_SCRATCH_SIZE 64
#endif

#ifndef FFFS_FILE_CACHE_SIZE
#define FFFS_FILE_CACHE_SIZE 256
#endif

#ifndef FFFS_FILE_WRITE_BUFFER
#define FFFS_FILE_WRITE_BUFFER FFFS_FILE_CACHE_SIZE
#endif

#if FFFS_FILE_CACHE_SIZE < 1
#error "FFFS_FILE_CACHE_SIZE must be greater than zero"
#endif

#ifndef FFFS_LAZY_DELETE_TOMBSTONES
#define FFFS_LAZY_DELETE_TOMBSTONES 0
#endif

/* Bytes of caller-owned mount cache storage required for this cache mode. */
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_NONE
#define FFFS_INDEX_CACHE_BYTES(count) 0
#elif FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS
#define FFFS_INDEX_CACHE_BYTES(count) ((count) * 4u)
#else
#define FFFS_INDEX_CACHE_BYTES(count) ((count) * 2u)
#endif

#endif
