/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS flash backend boundary, profiling hooks, aligned programming, and
 * scratch-buffer invalidation.
 */

#include "fffs_internal.h"

#include <string.h>

int fffs_map_backend_status(int status) {
    return status == 0 ? FFFS_OK : FFFS_ERR_IO;
}

bool fffs_valid_backend(const struct fffs_backend *backend) {
    return backend && backend->size &&
        backend->read_granule && backend->program_granule &&
        backend->program_granule <= FFFS_FILE_CACHE_SIZE &&
        backend->size % backend->program_granule == 0 &&
        backend->program_granule % backend->read_granule == 0 &&
        backend->read && backend->program && backend->erase;
}

bool fffs_flash_bytes_erased(const void *bytes, size_t size) {
    const uint8_t *p = bytes;
    for (size_t i = 0; i < size; i++) {
        if (p[i] != 0xff) {
            return false;
        }
    }
    return true;
}

#if FFFS_PROFILE_TRACE
void fffs_profile_push(struct fffs *fs, enum fffs_profile_scope scope) {
    if (!fs || fs->profile_depth >=
            sizeof(fs->profile_stack) / sizeof(fs->profile_stack[0])) {
        return;
    }
    fs->profile_stack[fs->profile_depth++] = scope;
}

void fffs_profile_pop(struct fffs *fs, enum fffs_profile_scope scope) {
    if (!fs || fs->profile_depth == 0) {
        return;
    }
    if (fs->profile_stack[fs->profile_depth - 1u] == scope) {
        fs->profile_depth--;
        return;
    }
    fs->profile_depth = 0;
}

void fffs_profile_flash(struct fffs *fs, enum fffs_profile_flash_op op,
        size_t offset, size_t size) {
    if (!fs || !fs->profile_trace) {
        return;
    }
    enum fffs_profile_scope unscoped = FFFS_PROFILE_UNSCOPED;
    const enum fffs_profile_scope *stack = fs->profile_depth == 0 ?
        &unscoped : fs->profile_stack;
    size_t depth = fs->profile_depth == 0 ? 1u : fs->profile_depth;
    fs->profile_trace(fs, stack, depth, op, offset, size,
            fs->profile_trace_user);
}
#endif

int fffs_flash_read(struct fffs *fs, size_t offset,
        void *buffer, size_t size) {
    int err = fffs_map_backend_status(fs->backend.read(fs->backend.ctx,
                offset, buffer, size));
#if FFFS_PROFILE_TRACE
    fffs_profile_flash(fs, FFFS_PROFILE_FLASH_READ, offset, size);
#endif
    return err;
}

int fffs_flash_program(struct fffs *fs, size_t offset,
        const void *buffer, size_t size) {
    int err = fffs_map_backend_status(fs->backend.program(fs->backend.ctx,
                offset, buffer, size));
#if FFFS_PROFILE_TRACE
    fffs_profile_flash(fs, FFFS_PROFILE_FLASH_PROGRAM, offset, size);
#endif
    return err;
}

int fffs_backend_program_aligned(const struct fffs_backend *backend,
        size_t offset, const void *buffer, size_t size) {
    if (size == 0) {
        return FFFS_OK;
    }
    size_t granule = backend->program_granule;
    if (offset % granule == 0 && size % granule == 0) {
        return fffs_map_backend_status(backend->program(backend->ctx,
                    offset, buffer, size));
    }

    uint8_t chunk[FFFS_FILE_CACHE_SIZE];
    const uint8_t *src = buffer;
    size_t done = 0;
    while (done < size) {
        size_t pos = offset + done;
        size_t base = pos - (pos % granule);
        size_t in_chunk = pos - base;
        size_t n = granule - in_chunk;
        if (n > size - done) {
            n = size - done;
        }

        memset(chunk, 0xff, granule);
        memcpy(chunk + in_chunk, src + done, n);
        int err = fffs_map_backend_status(backend->program(backend->ctx,
                    base, chunk, granule));
        if (err != FFFS_OK) {
            return err;
        }
        done += n;
    }
    return FFFS_OK;
}

int fffs_flash_program_aligned(struct fffs *fs, size_t offset,
        const void *buffer, size_t size) {
    int err = fffs_backend_program_aligned(&fs->backend, offset, buffer, size);
#if FFFS_PROFILE_TRACE
    fffs_profile_flash(fs, FFFS_PROFILE_FLASH_PROGRAM, offset, size);
#endif
    return err;
}

void fffs_scratch_bump(struct fffs *fs) {
    fs->scratch_serial++;
    if (fs->scratch_serial == 0) {
        fs->scratch_serial = 1;
    }
}
