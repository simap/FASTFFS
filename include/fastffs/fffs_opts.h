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
 * FFFS_INDEX_CACHE_HASH_HEADS stores only head sectors in a caller-provided
 * power-of-two hash table. The resolved slot key is recovered from root
 * metadata when hash collisions occur.
 *
 * FFFS_INDEX_CACHE_FULL_SLOT_HEADS stores one head sector per possible
 * resolved slot. It uses 128 KiB but provides direct slot lookup.
 */
#define FFFS_INDEX_CACHE_HASH_HEADS 1
#define FFFS_INDEX_CACHE_FULL_SLOT_HEADS 2

#ifndef FFFS_INDEX_CACHE_MODE
#define FFFS_INDEX_CACHE_MODE FFFS_INDEX_CACHE_HASH_HEADS
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

#ifndef FFFS_INDEX_REPLAY_FALLBACK_SIZE
#define FFFS_INDEX_REPLAY_FALLBACK_SIZE 64
#endif

#ifndef FFFS_INDEX_REPLAY_CHUNK_SIZE
#define FFFS_INDEX_REPLAY_CHUNK_SIZE 256
#endif

#ifndef FFFS_LAZY_DELETE_TOMBSTONES
#define FFFS_LAZY_DELETE_TOMBSTONES 0
#endif

#endif
