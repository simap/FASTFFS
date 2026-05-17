/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS internal mirrored-bit helpers for NOR-style state fields. The helper
 * classifies only whether all masked bits agree; format-specific code assigns
 * semantic meaning to set, cleared, or mixed states.
 */

#ifndef FASTFFS_FFFS_BITMIRROR_H
#define FASTFFS_FFFS_BITMIRROR_H

#include <stdint.h>

enum fffs_bitmirror_state {
    FFFS_BITMIRROR_SET,
    FFFS_BITMIRROR_CLEARED,
    FFFS_BITMIRROR_MIXED,
};

static inline enum fffs_bitmirror_state fffs_bitmirror_state(
        uint32_t value, uint32_t mask) {
    uint32_t set = value & mask;
    if (mask != 0 && set == mask) {
        return FFFS_BITMIRROR_SET;
    }
    if (mask != 0 && set == 0) {
        return FFFS_BITMIRROR_CLEARED;
    }
    return FFFS_BITMIRROR_MIXED;
}

#endif
