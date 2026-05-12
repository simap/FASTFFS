/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS core flash IO and on-flash record handling: index headers, index
 * replay/rotation, sector footers, and baseline metadata encoding.
 */

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
        uint8_t *index_sectors, uint8_t *sector_shift, uint8_t *serial) {
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
    uint8_t shift = hdr[6];
    if (shift < FFFS_MIN_SECTOR_SHIFT || shift > FFFS_MAX_SECTOR_SHIFT) {
        return false;
    }
    *index_sectors = count;
    *sector_shift = shift;
    *serial = (uint8_t)(hdr[5] & 0x0f);
    return true;
}

int fffs_program_index_header(const struct fffs_backend *backend,
        size_t offset, uint8_t index_sectors, uint8_t sector_shift,
        uint8_t serial) {
    uint8_t hdr[FFFS_HEADER_SIZE] = {
        'F', 'F', 'F', 'S',
        FFFS_INDEX_VERSION,
        (uint8_t)((index_sectors << 4) | (serial & 0x0f)),
        sector_shift,
        FFFS_INDEX_FLAGS_VALID,
    };
    return fffs_map_backend_status(backend->program(backend->ctx, offset,
                hdr, sizeof(hdr)));
}

int fffs_find_active_index_header(const struct fffs_backend *backend,
        size_t *active, uint8_t *index_sectors, uint8_t *sector_shift,
        uint8_t *serial) {
    uint8_t hdr[FFFS_HEADER_SIZE];
    bool found = false;
    uint8_t best_serial = 0;
    uint8_t best_index_sectors = 0;
    uint8_t best_sector_shift = 0;
    size_t best_active = 0;

    for (size_t off = 0; off < backend->size; off += FFFS_DEFAULT_SECTOR_SIZE) {
        int err = fffs_map_backend_status(backend->read(backend->ctx, off,
                    hdr, sizeof(hdr)));
        if (err != FFFS_OK) {
            return err;
        }
        uint8_t candidate_index_sectors;
        uint8_t candidate_sector_shift;
        uint8_t candidate_serial;
        if (!fffs_valid_index_header(hdr, &candidate_index_sectors,
                    &candidate_sector_shift, &candidate_serial)) {
            continue;
        }
        size_t sector_size = (size_t)256u << candidate_sector_shift;
        if (backend->size % sector_size != 0 || off % sector_size != 0) {
            continue;
        }
        size_t candidate_active = off / sector_size;
        if (candidate_active >= candidate_index_sectors) {
            continue;
        }
        if (!found || (candidate_serial != best_serial &&
                    ((candidate_serial - best_serial) & 0x0f) < 8)) {
            found = true;
            best_serial = candidate_serial;
            best_index_sectors = candidate_index_sectors;
            best_sector_shift = candidate_sector_shift;
            best_active = candidate_active;
        }
    }
    if (!found) {
        return FFFS_ERR_CORRUPT;
    }
    *active = best_active;
    *index_sectors = best_index_sectors;
    *sector_shift = best_sector_shift;
    *serial = best_serial;
    return FFFS_OK;
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

static int program_index_record(struct fffs *fs, size_t offset,
        uint16_t slot, uint16_t head) {
    uint8_t rec[4];
    store16(rec, slot);
    store16(rec + 2, head);
    return fffs_flash_program(fs, offset, rec, sizeof(rec));
}

int fffs_append_index_record(struct fffs *fs, uint16_t slot,
        uint16_t head) {
    if (fs->next_index_offset + 4 >
            (fs->active_index_sector + 1) * fs->sector_size) {
        int err = fffs_rotate_index(fs);
        if (err != FFFS_OK) {
            return err;
        }
        if (fs->next_index_offset + 4 >
                (fs->active_index_sector + 1) * fs->sector_size) {
            return FFFS_ERR_NO_SPACE;
        }
    }

    int err = program_index_record(fs, fs->next_index_offset, slot, head);
    if (err != FFFS_OK) {
        return err;
    }
    fs->next_index_offset += 4;
    return fffs_index_set(fs, slot, head);
}

static int compact_index_entry(struct fffs *fs, size_t *offset,
        uint16_t slot, uint16_t head, size_t sector_end) {
    if (*offset + 4 > sector_end) {
        return FFFS_ERR_NO_SPACE;
    }
    int err = program_index_record(fs, *offset, slot, head);
    if (err != FFFS_OK) {
        return err;
    }
    *offset += 4;
    return FFFS_OK;
}

int fffs_rotate_index(struct fffs *fs) {
    size_t old_active = fs->active_index_sector;
    size_t new_active = old_active + 1;
    if (new_active >= fs->index_sectors) {
        new_active = 0;
    }

    size_t new_base = new_active * fs->sector_size;
    size_t new_end = new_base + fs->sector_size;
    int err = fffs_map_backend_status(fs->backend.erase(fs->backend.ctx,
                new_base, fs->sector_size));
    if (err != FFFS_OK) {
        return err;
    }

    size_t off = new_base + FFFS_HEADER_SIZE;
#if FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_FULL_SLOT_HEADS
    for (size_t slot = 0; slot < FFFS_SLOT_COUNT; slot++) {
        uint16_t head = fs->index_heads[slot];
        if (head == 0) {
            continue;
        }
        err = compact_index_entry(fs, &off, (uint16_t)slot, head, new_end);
        if (err != FFFS_OK) {
            return err;
        }
    }
#elif FFFS_INDEX_CACHE_MODE == FFFS_INDEX_CACHE_HASH_HEADS
    for (size_t i = 0; i < fs->index_head_count; i++) {
        uint16_t head = fs->index_heads[i];
        if (head == 0) {
            continue;
        }
        uint16_t slot;
        err = fffs_read_metadata(fs, head, NULL, &slot, NULL, NULL);
        if (err != FFFS_OK) {
            return err;
        }
        err = compact_index_entry(fs, &off, slot, head, new_end);
        if (err != FFFS_OK) {
            return err;
        }
    }
#else
#error "Unsupported FFFS_INDEX_CACHE_MODE"
#endif

    uint8_t serial = (uint8_t)((fs->active_index_serial + 1) & 0x0f);
    err = fffs_program_index_header(&fs->backend, new_base, fs->index_sectors,
            fs->sector_shift, serial);
    if (err != FFFS_OK) {
        return err;
    }

    uint8_t tombstone[4] = {
        FFFS_INDEX_VERSION,
        (uint8_t)((fs->index_sectors << 4) |
            (fs->active_index_serial & 0x0f)),
        fs->sector_shift,
        (uint8_t)(FFFS_INDEX_FLAGS_VALID &
            (uint8_t)~FFFS_INDEX_FLAG_TOMBSTONED),
    };
    err = fffs_flash_program(fs, old_active * fs->sector_size + 4,
            tombstone, sizeof(tombstone));
    if (err != FFFS_OK) {
        return err;
    }

    fs->active_index_sector = new_active;
    fs->active_index_serial = serial;
    fs->next_index_offset = off;
    return FFFS_OK;
}

static void recover_allocator_hint(struct fffs *fs, uint16_t head) {
    size_t sector = head;

    fs->alloc_cursor = fffs_next_data_sector(fs, head);

    for (size_t checked = 0; checked <= FFFS_ALLOC_RECOVERY_LOOKAHEAD;
            checked++) {
        uint32_t serial;
        int err;

        if (sector < fs->index_sectors || sector >= fs->sector_count) {
            return;
        }

        err = fffs_read_sector_footer(fs, (uint16_t)sector, &serial);
        if (err == FFFS_OK) {
            if (serial >= fs->next_sector_serial) {
                fs->next_sector_serial = serial + 1;
                if (fs->next_sector_serial == 0) {
                    fs->next_sector_serial = 1;
                }
            }
        }

        size_t next = fffs_next_data_sector(fs, sector);
        if (next <= sector) {
            return;
        }
        sector = next;
    }
}

int fffs_replay_index(struct fffs *fs) {
    size_t end = (fs->active_index_sector + 1) * fs->sector_size;
    uint16_t tail_head = 0;
    for (size_t off = fs->next_index_offset; off + 4 <= end; off += 4) {
        uint16_t slot;
        uint16_t head;
        int err = fffs_read_index_record(fs, off, &slot, &head);
        if (err != FFFS_OK) {
            return err;
        }
        if (slot == UINT16_MAX && head == UINT16_MAX) {
            fs->next_index_offset = off;
            if (tail_head != 0) {
                recover_allocator_hint(fs, tail_head);
            }
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
        err = fffs_index_insert(fs, slot, head);
        if (err != FFFS_OK) {
            return err;
        }
        tail_head = head;
    }

    fs->next_index_offset = end;
    if (tail_head != 0) {
        recover_allocator_hint(fs, tail_head);
    }
    return FFFS_OK;
}

size_t fffs_max_file_data_size(const struct fffs *fs) {
    return fs->sector_size - FFFS_SECTOR_FOOTER_SIZE - FFFS_MD_SIZE;
}

static size_t sector_footer_offset(struct fffs *fs, uint16_t sector) {
    return (size_t)sector * fs->sector_size + fs->sector_size -
        FFFS_SECTOR_FOOTER_SIZE;
}

static size_t sector_metadata_offset(struct fffs *fs, uint16_t sector) {
    return sector_footer_offset(fs, sector) - FFFS_MD_SIZE;
}

int fffs_read_sector_footer(struct fffs *fs, uint16_t sector,
        uint32_t *serial) {
    if (sector < fs->index_sectors || sector >= fs->sector_count) {
        return FFFS_ERR_CORRUPT;
    }

    uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
    int err = fffs_flash_read(fs, sector_footer_offset(fs, sector),
            footer, sizeof(footer));
    if (err != FFFS_OK) {
        return err;
    }
    if (footer[4] != FFFS_SECTOR_TYPE_MIXED ||
            footer[5] != FFFS_SECTOR_FLAGS_VALID ||
            footer[6] != 0xff || footer[7] != 0xff ||
            memcmp(footer + 8, FFFS_SECTOR_MAGIC, 4) != 0) {
        return FFFS_ERR_CORRUPT;
    }
    if (serial) {
        *serial = load32(footer);
    }
    return FFFS_OK;
}

int fffs_write_sector_footer(struct fffs_file *file) {
    struct fffs *fs = file->fs;
    uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
    memset(footer, 0xff, sizeof(footer));
    store32(footer, file->sector_serial);
    footer[4] = FFFS_SECTOR_TYPE_MIXED;
    footer[5] = FFFS_SECTOR_FLAGS_VALID;
    memcpy(footer + 8, FFFS_SECTOR_MAGIC, 4);
    return fffs_flash_program(fs, sector_footer_offset(fs, file->head),
            footer, sizeof(footer));
}

int fffs_read_metadata(struct fffs *fs, uint16_t sector,
        struct fffs_stat *st, uint16_t *slot, uint16_t *data_off,
        uint16_t *data_len) {
    if (sector < fs->index_sectors || sector >= fs->sector_count) {
        return FFFS_ERR_CORRUPT;
    }

    int err = fffs_read_sector_footer(fs, sector, NULL);
    if (err != FFFS_OK) {
        return err;
    }

    uint8_t md[FFFS_MD_SIZE];
    err = fffs_flash_read(fs, sector_metadata_offset(fs, sector),
            md, sizeof(md));
    if (err != FFFS_OK) {
        return err;
    }
    if (md[0] != FFFS_MD_FLAGS_VALID ||
            md[63] != FFFS_MD_TYPE_BASELINE ||
            md[1] == 0 || md[1] > FFFS_MAX_NAME) {
        return FFFS_ERR_CORRUPT;
    }

    uint16_t off = load16(md + 4);
    uint16_t len = load16(md + 6);
    if ((size_t)off + len > fffs_max_file_data_size(fs)) {
        return FFFS_ERR_CORRUPT;
    }

    if (slot) {
        *slot = load16(md + 2);
    }
    if (data_off) {
        *data_off = off;
    }
    if (data_len) {
        *data_len = len;
    }
    if (st) {
        size_t name_len = md[1];
        memcpy(st->name, md + 14, name_len);
        st->name[name_len] = '\0';
        st->size = load32(md + 8);
    }
    return FFFS_OK;
}

int fffs_write_root_metadata(struct fffs_file *file) {
    struct fffs *fs = file->fs;
    uint8_t md[FFFS_MD_SIZE];
    memset(md, 0xff, sizeof(md));
    md[0] = FFFS_MD_FLAGS_VALID;
    md[1] = (uint8_t)strlen(file->name);
    store16(md + 2, file->slot);
    store16(md + 4, file->data_offset);
    store16(md + 6, (uint16_t)file->size);
    store32(md + 8, file->size);
    store16(md + 12, 0);
    memcpy(md + 14, file->name, md[1]);
    md[63] = FFFS_MD_TYPE_BASELINE;

    int err = fffs_write_sector_footer(file);
    if (err != FFFS_OK) {
        return err;
    }
    err = fffs_flash_program(fs, sector_metadata_offset(fs, file->head),
            md, sizeof(md));
    if (err != FFFS_OK) {
        return err;
    }
    return fffs_append_index_record(fs, file->slot, file->head);
}
