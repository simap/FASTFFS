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

#ifndef FFFS_INDEX_HASH_HEAD_COUNT
#define FFFS_INDEX_HASH_HEAD_COUNT 1024
#endif

#ifndef FFFS_INDEX_HASH_HEAD_COUNT_MAX
#define FFFS_INDEX_HASH_HEAD_COUNT_MAX 16384
#endif

#ifndef FFFS_ALLOC_RECOVERY_LOOKAHEAD
#define FFFS_ALLOC_RECOVERY_LOOKAHEAD 4
#endif

#endif
