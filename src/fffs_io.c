#include "fffs_internal.h"

#include <string.h>

static uint16_t load16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t load32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void store32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int fffs_map_backend_status(int status) {
    return status == 0 ? FFFS_OK : FFFS_ERR_IO;
}

bool fffs_valid_backend(const struct fffs_backend *backend) {
    return backend && backend->ctx && backend->size &&
        backend->read_granule && backend->program_granule &&
        backend->program_granule <= FFFS_MAX_PROGRAM_GRANULE &&
        backend->read && backend->program && backend->erase;
}

uint16_t fffs_hash16(const char *name) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return (uint16_t)((h >> 16) ^ h);
}

int fffs_flash_read(struct fffs *fs, size_t offset,
        void *buffer, size_t size) {
    return fffs_map_backend_status(fs->backend.read(fs->backend.ctx,
                offset, buffer, size));
}

int fffs_flash_program(struct fffs *fs, size_t offset,
        const void *buffer, size_t size) {
    return fffs_map_backend_status(fs->backend.program(fs->backend.ctx,
                offset, buffer, size));
}

bool fffs_valid_index_header(const uint8_t hdr[FFFS_HEADER_SIZE],
        uint8_t *index_sectors, uint8_t *sector_shift) {
    if (memcmp(hdr, FFFS_INDEX_MAGIC, 4) != 0 ||
            hdr[4] != FFFS_INDEX_VERSION) {
        return false;
    }
    if ((hdr[7] & FFFS_INDEX_FLAG_VALID) != 0 ||
            (hdr[7] & FFFS_INDEX_FLAG_TOMBSTONED) == 0 ||
            (hdr[7] & FFFS_INDEX_FLAG_MD_CRC_REQUIRED) == 0 ||
            (hdr[7] & 0x1f) != 0x1f) {
        return false;
    }
    uint8_t count = (uint8_t)(hdr[5] >> 4);
    if (count < 2 || count > 15) {
        return false;
    }
    *index_sectors = count;
    *sector_shift = hdr[6];
    return true;
}

int fffs_program_index_header(const struct fffs_backend *backend,
        uint8_t index_sectors, uint8_t sector_shift) {
    uint8_t hdr[FFFS_HEADER_SIZE] = {
        'F', 'F', 'F', 'S',
        FFFS_INDEX_VERSION,
        (uint8_t)(index_sectors << 4),
        sector_shift,
        FFFS_INDEX_FLAGS_VALID,
    };
    return fffs_map_backend_status(backend->program(backend->ctx, 0, hdr,
                sizeof(hdr)));
}

int fffs_find_active_index_header(const struct fffs_backend *backend,
        size_t *active, uint8_t *index_sectors, uint8_t *sector_shift) {
    uint8_t hdr[FFFS_HEADER_SIZE];
    for (size_t off = 0; off < backend->size;
            off += FFFS_DEFAULT_SECTOR_SIZE) {
        int err = fffs_map_backend_status(backend->read(backend->ctx, off,
                    hdr, sizeof(hdr)));
        if (err != FFFS_OK) {
            return err;
        }
        if (fffs_valid_index_header(hdr, index_sectors, sector_shift)) {
            *active = off / FFFS_DEFAULT_SECTOR_SIZE;
            return FFFS_OK;
        }
    }
    return FFFS_ERR_CORRUPT;
}

int fffs_read_index_record(struct fffs *fs, size_t offset,
        uint16_t *slot, uint16_t *head) {
    uint8_t rec[4];
    int err = fffs_flash_read(fs, offset, rec, sizeof(rec));
    if (err != FFFS_OK) {
        return err;
    }
    *slot = load16(rec);
    *head = load16(rec + 2);
    return FFFS_OK;
}

int fffs_append_index_record(struct fffs *fs, uint16_t slot,
        uint16_t head) {
    if (fs->next_index_offset + 4 >
            (fs->active_index_sector + 1) * fs->sector_size) {
        return FFFS_ERR_NO_SPACE;
    }

    uint8_t rec[4];
    store16(rec, slot);
    store16(rec + 2, head);
    int err = fffs_flash_program(fs, fs->next_index_offset,
            rec, sizeof(rec));
    if (err != FFFS_OK) {
        return err;
    }
    fs->next_index_offset += 4;
    return fffs_index_set(fs, slot, head);
}

int fffs_replay_index(struct fffs *fs) {
    size_t end = (fs->active_index_sector + 1) * fs->sector_size;
    for (size_t off = fs->next_index_offset; off + 4 <= end; off += 4) {
        uint16_t slot;
        uint16_t head;
        int err = fffs_read_index_record(fs, off, &slot, &head);
        if (err != FFFS_OK) {
            return err;
        }
        if (slot == UINT16_MAX && head == UINT16_MAX) {
            fs->next_index_offset = off;
            return FFFS_OK;
        }
        if (slot == 0 && head == 0) {
            return FFFS_ERR_CORRUPT;
        }
        if (head == 0) {
            err = fffs_index_remove(fs, slot);
            if (err != FFFS_OK) {
                return err;
            }
            continue;
        }
        if (head < fs->index_sectors || head >= fs->sector_count) {
            return FFFS_ERR_CORRUPT;
        }

        uint16_t md_slot;
        err = fffs_read_metadata(fs, head, NULL, &md_slot, NULL, NULL);
        if (err != FFFS_OK || md_slot != slot) {
            return FFFS_ERR_CORRUPT;
        }
        err = fffs_index_insert(fs, slot, head);
        if (err != FFFS_OK) {
            return err;
        }
    }

    fs->next_index_offset = end;
    return FFFS_OK;
}

size_t fffs_max_file_data_size(const struct fffs *fs) {
    return fs->sector_size - FFFS_MD_SIZE;
}

int fffs_read_metadata(struct fffs *fs, uint16_t sector,
        struct fffs_stat *st, uint16_t *slot, uint16_t *data_off,
        uint16_t *data_len) {
    if (sector < fs->index_sectors || sector >= fs->sector_count) {
        return FFFS_ERR_CORRUPT;
    }

    uint8_t md[FFFS_MD_SIZE];
    size_t offset = (size_t)sector * fs->sector_size +
        fs->sector_size - FFFS_MD_SIZE;
    int err = fffs_flash_read(fs, offset, md, sizeof(md));
    if (err != FFFS_OK) {
        return err;
    }
    if (memcmp(md, FFFS_MD_MAGIC, 4) != 0 ||
            md[4] != FFFS_MD_FLAGS_VALID ||
            md[63] != FFFS_MD_TYPE_BASELINE ||
            md[5] == 0 || md[5] > FFFS_MAX_NAME) {
        return FFFS_ERR_CORRUPT;
    }

    uint16_t off = load16(md + 8);
    uint16_t len = load16(md + 10);
    if ((size_t)off + len > fs->sector_size - FFFS_MD_SIZE) {
        return FFFS_ERR_CORRUPT;
    }

    if (slot) {
        *slot = load16(md + 6);
    }
    if (data_off) {
        *data_off = off;
    }
    if (data_len) {
        *data_len = len;
    }
    if (st) {
        size_t name_len = md[5];
        memcpy(st->name, md + 18, name_len);
        st->name[name_len] = '\0';
        st->size = load32(md + 12);
    }
    return FFFS_OK;
}

int fffs_write_root_metadata(struct fffs_file *file) {
    struct fffs *fs = file->fs;
    uint8_t md[FFFS_MD_SIZE];
    memset(md, 0xff, sizeof(md));
    memcpy(md, FFFS_MD_MAGIC, 4);
    md[4] = FFFS_MD_FLAGS_VALID;
    md[5] = (uint8_t)strlen(file->name);
    store16(md + 6, file->slot);
    store16(md + 8, file->data_offset);
    store16(md + 10, (uint16_t)file->size);
    store32(md + 12, file->size);
    store16(md + 16, 0);
    memcpy(md + 18, file->name, md[5]);
    md[63] = FFFS_MD_TYPE_BASELINE;

    int err = fffs_flash_program(fs, (size_t)file->head * fs->sector_size +
            fs->sector_size - FFFS_MD_SIZE, md, sizeof(md));
    if (err != FFFS_OK) {
        return err;
    }
    return fffs_append_index_record(fs, file->slot, file->head);
}
