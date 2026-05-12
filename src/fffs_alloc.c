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

static bool sector_is_live_head(struct fffs *fs, size_t sector) {
    for (size_t i = 0; i < fs->index_head_count; i++) {
        if (fs->index_heads[i] == sector) {
            return true;
        }
    }
    return false;
}

static int sector_is_index_referenced(struct fffs *fs, size_t sector,
        bool *referenced) {
    *referenced = false;
    size_t end = (fs->active_index_sector + 1) * fs->sector_size;
    size_t off = fs->active_index_sector * fs->sector_size + FFFS_HEADER_SIZE;
    for (; off + 4 <= end; off += 4) {
        uint16_t slot;
        uint16_t head;
        int err = fffs_read_index_record(fs, off, &slot, &head);
        if (err != FFFS_OK) {
            return err;
        }
        if (slot == UINT16_MAX && head == UINT16_MAX) {
            return FFFS_OK;
        }
        if (slot == 0 && head == 0) {
            return FFFS_ERR_CORRUPT;
        }
        if (head == sector) {
            *referenced = true;
            return FFFS_OK;
        }
    }
    return FFFS_OK;
}

size_t fffs_next_data_sector(struct fffs *fs, size_t sector) {
    sector++;
    if (sector < fs->index_sectors || sector >= fs->sector_count) {
        return fs->index_sectors;
    }
    return sector;
}

int fffs_find_free_sector(struct fffs *fs, uint16_t *sector) {
    if (fs->sector_count <= fs->index_sectors) {
        return FFFS_ERR_NO_SPACE;
    }

    size_t s = fs->alloc_cursor;
    if (s < fs->index_sectors || s >= fs->sector_count) {
        s = fs->index_sectors;
    }

    size_t data_sectors = fs->sector_count - fs->index_sectors;
    for (size_t checked = 0; checked < data_sectors; checked++) {
        if (sector_is_live_head(fs, s)) {
            s = fffs_next_data_sector(fs, s);
            continue;
        }
        int err = flash_span_is_erased(fs, s * fs->sector_size,
                fs->sector_size);
        if (err == FFFS_OK) {
            *sector = (uint16_t)s;
            fs->alloc_cursor = fffs_next_data_sector(fs, s);
            return FFFS_OK;
        }
        if (err != FFFS_ERR_NO_SPACE) {
            return err;
        }
        s = fffs_next_data_sector(fs, s);
    }
    return FFFS_ERR_NO_SPACE;
}

int fffs_gc(struct fffs *fs, size_t max_sectors, size_t *out_erased) {
    if (!fs) {
        return FFFS_ERR_INVALID;
    }
    if (out_erased) {
        *out_erased = 0;
    }
    if (fs->sector_count <= fs->index_sectors || max_sectors == 0) {
        return FFFS_OK;
    }

    size_t s = fs->gc_cursor;
    if (s < fs->index_sectors || s >= fs->sector_count) {
        s = fs->index_sectors;
    }

    size_t data_sectors = fs->sector_count - fs->index_sectors;
    size_t limit = max_sectors < data_sectors ? max_sectors : data_sectors;
    size_t erased = 0;
    for (size_t checked = 0; checked < limit; checked++) {
        if (!sector_is_live_head(fs, s)) {
            bool referenced;
            int err = sector_is_index_referenced(fs, s, &referenced);
            if (err != FFFS_OK) {
                return err;
            }
            if (referenced) {
                s = fffs_next_data_sector(fs, s);
                fs->gc_cursor = s;
                continue;
            }

            err = flash_span_is_erased(fs, s * fs->sector_size,
                    fs->sector_size);
            if (err == FFFS_ERR_NO_SPACE) {
                err = fffs_read_sector_footer(fs, (uint16_t)s, NULL);
                if (err != FFFS_OK) {
                    return err;
                }
                err = fffs_map_backend_status(fs->backend.erase(
                            fs->backend.ctx, s * fs->sector_size,
                            fs->sector_size));
                if (err != FFFS_OK) {
                    return err;
                }
                erased += 1;
                if (out_erased) {
                    *out_erased = erased;
                }
            } else if (err != FFFS_OK) {
                return err;
            }
        }
        s = fffs_next_data_sector(fs, s);
        fs->gc_cursor = s;
    }
    return FFFS_OK;
}
