/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS internal fixed-storage bitset helpers used by low-memory index
 * algorithms that need compact caller-owned temporary state.
 */

#include "fffs_internal.h"

#include <string.h>

enum {
    FFFS_BITSET_WORD_BITS = 32u,
};

void fffs_bitset_clear(uint32_t *words, size_t word_count) {
    memset(words, 0, word_count * sizeof(words[0]));
}

bool fffs_bitset_get(const uint32_t *words, size_t bit) {
    return (words[bit / FFFS_BITSET_WORD_BITS] &
        ((uint32_t)1u << (bit % FFFS_BITSET_WORD_BITS))) != 0;
}

void fffs_bitset_set(uint32_t *words, size_t bit) {
    words[bit / FFFS_BITSET_WORD_BITS] |=
        (uint32_t)1u << (bit % FFFS_BITSET_WORD_BITS);
}
