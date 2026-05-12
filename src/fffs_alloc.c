#include "fffs_internal.h"

#define FFFS_ERASED_CHECK_CHUNK 64

static int flash_span_is_erased(struct fffs *fs, size_t offset, size_t size) {
    uint8_t chunk[FFFS_ERASED_CHECK_CHUNK];
    while (size > 0) {
        size_t n = size < sizeof(chunk) ? size : sizeof(chunk);
        int err = fffs_flash_read(fs, offset, chunk, n);
        if (err != FFFS_OK) {
            return err;
        }
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

int fffs_find_free_sector(struct fffs *fs, uint16_t *sector) {
    for (size_t s = fs->index_sectors; s < fs->sector_count; s++) {
        bool live_head = false;
        for (size_t i = 0; i < fs->index_head_count; i++) {
            if (fs->index_heads[i] == s) {
                live_head = true;
                break;
            }
        }
        if (live_head) {
            continue;
        }
        if (flash_span_is_erased(fs, s * fs->sector_size,
                    fs->sector_size) == FFFS_OK) {
            *sector = (uint16_t)s;
            return FFFS_OK;
        }
    }
    return FFFS_ERR_NO_SPACE;
}
