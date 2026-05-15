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
 * FFFS_INDEX_CACHE_HASH_HEADS stores only head sectors in a caller-provided
 * power-of-two hash table. The resolved slot key is recovered from root
 * metadata when hash collisions occur.
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
#define FFFS_ALLOC_MAP_MODE FFFS_ALLOC_MAP_NONE
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

#ifndef FFFS_GC_ON_ALLOC_FAILURE
#define FFFS_GC_ON_ALLOC_FAILURE 1
#endif

#ifndef FFFS_INDEX_COMPACT_OUTER_SCAN_SIZE
#define FFFS_INDEX_COMPACT_OUTER_SCAN_SIZE 64
#endif

#ifndef FFFS_INDEX_COMPACT_WRITE_BUFFER_SIZE
#define FFFS_INDEX_COMPACT_WRITE_BUFFER_SIZE 64
#endif

#ifndef FFFS_MIN_SCRATCH_SIZE
#define FFFS_MIN_SCRATCH_SIZE 64
#endif

#ifndef FFFS_LAZY_DELETE_TOMBSTONES
#define FFFS_LAZY_DELETE_TOMBSTONES 0
#endif

#endif
