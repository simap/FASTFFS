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
        backend->size % backend->program_granule == 0 &&
        backend->program_granule % backend->read_granule == 0 &&
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

uint16_t fffs_normalize_slot_base(uint16_t slot) {
    if (slot == 0) {
        return 1;
    }
    if (slot == UINT16_MAX) {
        return 0x7fff;
    }
    return slot;
}

uint16_t fffs_next_slot(uint16_t slot) {
    slot++;
    if (slot == 0 || slot == UINT16_MAX) {
        return 1;
    }
    return slot;
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

static int backend_program_aligned(const struct fffs_backend *backend,
        size_t offset, const void *buffer, size_t size) {
    if (size == 0) {
        return FFFS_OK;
    }
    size_t granule = backend->program_granule;
    if (offset % granule == 0 && size % granule == 0) {
        return fffs_map_backend_status(backend->program(backend->ctx,
                    offset, buffer, size));
    }

    uint8_t chunk[FFFS_MAX_PROGRAM_GRANULE];
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
    return backend_program_aligned(&fs->backend, offset, buffer, size);
}

void fffs_scratch_bump(struct fffs *fs) {
    fs->scratch_serial++;
    if (fs->scratch_serial == 0) {
        fs->scratch_serial = 1;
    }
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
    return backend_program_aligned(backend, offset, hdr, sizeof(hdr));
}

static bool index_serial_newer(uint8_t serial, uint8_t best_serial) {
    return serial != best_serial && ((serial - best_serial) & 0x0f) < 8;
}

static int consider_index_header(size_t off,
        uint8_t candidate_index_sectors, uint8_t candidate_sector_shift,
        uint8_t candidate_serial, bool *found, size_t *best_active,
        uint8_t *best_serial) {
    size_t sector_size = (size_t)256u << candidate_sector_shift;
    if (off % sector_size != 0) {
        return FFFS_ERR_CORRUPT;
    }
    size_t candidate_active = off / sector_size;
    if (candidate_active >= candidate_index_sectors) {
        return FFFS_ERR_CORRUPT;
    }
    if (!*found || index_serial_newer(candidate_serial, *best_serial)) {
        *found = true;
        *best_serial = candidate_serial;
        *best_active = candidate_active;
    }
    return FFFS_OK;
}

static size_t prev_index_sector(size_t sector, size_t index_sectors) {
    return sector == 0 ? index_sectors - 1 : sector - 1;
}

static void find_oldest_index_sector(const bool *valid, const uint8_t *serials,
        size_t index_sectors, size_t active, uint8_t active_serial,
        size_t *oldest, size_t *count) {
    size_t current = active;
    uint8_t current_serial = active_serial;
    size_t n = 1;

    while (n < index_sectors) {
        size_t prev = prev_index_sector(current, index_sectors);
        uint8_t expected = (uint8_t)((current_serial - 1u) & 0x0f);
        if (!valid[prev] || serials[prev] != expected) {
            break;
        }
        current = prev;
        current_serial = serials[prev];
        n++;
    }

    *oldest = current;
    *count = n;
}

static bool discovery_probe_shift(size_t probe, uint8_t *shift) {
    if (probe == 0) {
        *shift = FFFS_DEFAULT_SECTOR_SHIFT;
        return true;
    }
    if (probe <= FFFS_DEFAULT_SECTOR_SHIFT - FFFS_MIN_SECTOR_SHIFT) {
        *shift = (uint8_t)(FFFS_DEFAULT_SECTOR_SHIFT - probe);
        return true;
    }
    uint8_t larger = (uint8_t)(FFFS_DEFAULT_SECTOR_SHIFT +
        (probe - (FFFS_DEFAULT_SECTOR_SHIFT - FFFS_MIN_SECTOR_SHIFT)));
    if (larger > FFFS_MAX_SECTOR_SHIFT) {
        return false;
    }
    *shift = larger;
    return true;
}

int fffs_find_index_sequence(const struct fffs_backend *backend,
        struct fffs_index_sequence *sequence) {
    if (!sequence) {
        return FFFS_ERR_INVALID;
    }
    uint8_t hdr[FFFS_HEADER_SIZE];
    for (size_t probe = 0;; probe++) {
        uint8_t probe_shift;
        if (!discovery_probe_shift(probe, &probe_shift)) {
            break;
        }
        size_t sector_size = (size_t)256u << probe_shift;
        if (backend->size % sector_size != 0) {
            continue;
        }

        for (size_t sector = 0; sector < 2; sector++) {
            size_t discovered_off = sector * sector_size;
            uint8_t discovered_hdr[FFFS_HEADER_SIZE];
            if (discovered_off + sizeof(hdr) > backend->size) {
                continue;
            }
            int err = fffs_map_backend_status(backend->read(backend->ctx,
                        discovered_off, hdr, sizeof(hdr)));
            if (err != FFFS_OK) {
                return err;
            }

            uint8_t locked_index_sectors;
            uint8_t locked_sector_shift;
            uint8_t candidate_serial;
            if (!fffs_valid_index_header(hdr, &locked_index_sectors,
                        &locked_sector_shift, &candidate_serial) ||
                    locked_sector_shift != probe_shift) {
                continue;
            }
            if (backend->size % sector_size != 0) {
                continue;
            }
            if (backend->size / sector_size < locked_index_sectors) {
                continue;
            }
            memcpy(discovered_hdr, hdr, sizeof(discovered_hdr));

            bool found = false;
            bool conflict = false;
            uint8_t best_serial = 0;
            size_t best_active = 0;
            bool valid[15] = {0};
            uint8_t serials[15] = {0};
            for (size_t i = 0; i < locked_index_sectors; i++) {
                size_t off = i * sector_size;
                if (off == discovered_off) {
                    memcpy(hdr, discovered_hdr, sizeof(discovered_hdr));
                } else {
                    err = fffs_map_backend_status(backend->read(
                                backend->ctx, off, hdr, sizeof(hdr)));
                    if (err != FFFS_OK) {
                        return err;
                    }
                }

                uint8_t candidate_index_sectors;
                uint8_t candidate_sector_shift;
                if (!fffs_valid_index_header(hdr,
                            &candidate_index_sectors,
                            &candidate_sector_shift, &candidate_serial)) {
                    continue;
                }
                if (candidate_index_sectors != locked_index_sectors ||
                        candidate_sector_shift != locked_sector_shift) {
                    conflict = true;
                    break;
                }
                valid[i] = true;
                serials[i] = candidate_serial;
                err = consider_index_header(off, candidate_index_sectors,
                        candidate_sector_shift, candidate_serial, &found,
                        &best_active, &best_serial);
                if (err != FFFS_OK) {
                    conflict = true;
                    break;
                }
            }
            if (conflict || !found) {
                continue;
            }

            size_t oldest;
            size_t count;
            find_oldest_index_sector(valid, serials, locked_index_sectors,
                    best_active, best_serial, &oldest, &count);

            *sequence = (struct fffs_index_sequence){
                .index_sectors = locked_index_sectors,
                .sector_shift = locked_sector_shift,
                .active_serial = best_serial,
                .active_sector = best_active,
                .oldest_sector = oldest,
                .count = count,
            };
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

static int program_index_record(struct fffs *fs, size_t offset,
        uint16_t slot, uint16_t head) {
    uint8_t rec[4];
    store16(rec, slot);
    store16(rec + 2, head);
    return fffs_flash_program_aligned(fs, offset, rec, sizeof(rec));
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

int fffs_compact_index_entry(struct fffs *fs, size_t *offset,
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
    err = fffs_index_compact(fs, &off, new_end);
    if (err != FFFS_OK) {
        return err;
    }

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
    err = fffs_flash_program_aligned(fs, old_active * fs->sector_size + 4,
            tombstone, sizeof(tombstone));
    if (err != FFFS_OK) {
        return err;
    }

    fs->active_index_sector = new_active;
    fs->active_index_serial = serial;
    fs->oldest_index_sector = new_active;
    fs->index_sequence_count = 1;
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
    uint16_t tail_head = 0;
    enum {
        replay_fallback_size =
            FFFS_INDEX_REPLAY_FALLBACK_SIZE < 64 ?
            64 : FFFS_INDEX_REPLAY_FALLBACK_SIZE,
        replay_chunk_size =
            FFFS_INDEX_REPLAY_CHUNK_SIZE < 64 ?
            64 : FFFS_INDEX_REPLAY_CHUNK_SIZE
    };
    uint8_t fallback[replay_fallback_size];
    uint8_t *chunk = fs->scratch ? fs->scratch : fallback;
    size_t chunk_size = fs->scratch ? fs->scratch_size : sizeof(fallback);
    if (chunk_size > replay_chunk_size) {
        chunk_size = replay_chunk_size;
    }
    chunk_size -= chunk_size % 4;
    if (chunk_size == 0) {
        chunk = fallback;
        chunk_size = sizeof(fallback);
    }

    fs->next_index_offset = fs->active_index_sector * fs->sector_size +
        FFFS_HEADER_SIZE;
    size_t sector = fs->oldest_index_sector;
    for (size_t order = 0; order < fs->index_sequence_count; order++) {
        size_t end = (sector + 1) * fs->sector_size;
        bool active = sector == fs->active_index_sector;
        for (size_t off = sector * fs->sector_size + FFFS_HEADER_SIZE;
                off + 4 <= end;) {
            size_t remaining = end - off;
            size_t nread = remaining < chunk_size ? remaining : chunk_size;
            nread -= nread % 4;
            int err = fffs_flash_read(fs, off, chunk, nread);
            if (err != FFFS_OK) {
                return err;
            }
            if (chunk == fs->scratch) {
                fffs_scratch_bump(fs);
            }
            for (size_t pos = 0; pos < nread; pos += 4) {
                uint16_t slot = load16(chunk + pos);
                uint16_t head = load16(chunk + pos + 2);
                size_t rec_off = off + pos;

                if (slot == UINT16_MAX && head == UINT16_MAX) {
                    if (active) {
                        fs->next_index_offset = rec_off;
                    }
                    goto next_sector;
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
                tail_head = head;
                err = fffs_index_insert(fs, slot, head);
                if (err != FFFS_OK) {
                    return err;
                }
            }
            off += nread;
            if (active) {
                fs->next_index_offset = off;
            }
        }
next_sector:
        sector++;
        if (sector >= fs->index_sectors) {
            sector = 0;
        }
    }
    if (tail_head != 0) {
        recover_allocator_hint(fs, tail_head);
    }
    return FFFS_OK;
}

size_t fffs_max_file_data_size(const struct fffs *fs) {
    size_t raw = fs->sector_size - FFFS_SECTOR_FOOTER_SIZE - FFFS_MD_SIZE;
    if (raw > UINT16_MAX) {
        raw = UINT16_MAX;
    }
    return raw - (raw % fs->backend.program_granule);
}

size_t fffs_sector_footer_offset(struct fffs *fs, uint16_t sector) {
    return (size_t)sector * fs->sector_size + fs->sector_size -
        FFFS_SECTOR_FOOTER_SIZE;
}

size_t fffs_sector_metadata_offset(struct fffs *fs, uint16_t sector) {
    return fffs_sector_footer_offset(fs, sector) - FFFS_MD_SIZE;
}

int fffs_read_sector_footer(struct fffs *fs, uint16_t sector,
        uint32_t *serial) {
    if (sector < fs->index_sectors || sector >= fs->sector_count) {
        return FFFS_ERR_CORRUPT;
    }

    uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
    int err = fffs_flash_read(fs, fffs_sector_footer_offset(fs, sector),
            footer, sizeof(footer));
    if (err != FFFS_OK) {
        return err;
    }
    if (footer[4] != FFFS_SECTOR_TYPE_MIXED ||
            (footer[5] != FFFS_SECTOR_FLAGS_VALID &&
             footer[5] != FFFS_SECTOR_FLAGS_TOMBSTONED) ||
            footer[6] != 0xff || footer[7] != 0xff ||
            memcmp(footer + 8, FFFS_SECTOR_MAGIC, 4) != 0) {
        return FFFS_ERR_CORRUPT;
    }
    if (serial) {
        *serial = load32(footer);
    }
    return FFFS_OK;
}

static void encode_sector_footer(uint8_t footer[FFFS_SECTOR_FOOTER_SIZE],
        uint32_t serial) {
    memset(footer, 0xff, FFFS_SECTOR_FOOTER_SIZE);
    store32(footer, serial);
    footer[4] = FFFS_SECTOR_TYPE_MIXED;
    footer[5] = FFFS_SECTOR_FLAGS_VALID;
    memcpy(footer + 8, FFFS_SECTOR_MAGIC, 4);
}

int fffs_write_sector_footer(struct fffs *fs, uint16_t sector,
        uint32_t serial) {
    uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
    encode_sector_footer(footer, serial);
    return fffs_flash_program_aligned(fs, fffs_sector_footer_offset(fs, sector),
            footer, sizeof(footer));
}

int fffs_tombstone_sector(struct fffs *fs, uint16_t sector) {
    uint8_t state[4] = {
        FFFS_SECTOR_TYPE_MIXED,
        FFFS_SECTOR_FLAGS_TOMBSTONED,
        0xff,
        0xff,
    };
    return fffs_flash_program_aligned(fs,
            fffs_sector_footer_offset(fs, sector) + 4, state, sizeof(state));
}

int fffs_read_metadata(struct fffs *fs, uint16_t sector,
        struct fffs_stat *st, uint16_t *slot, uint16_t *data_off,
        uint16_t *data_len, uint16_t *next) {
    if (sector < fs->index_sectors || sector >= fs->sector_count) {
        return FFFS_ERR_CORRUPT;
    }

    uint8_t tail[FFFS_MD_SIZE + FFFS_SECTOR_FOOTER_SIZE];
    int err = fffs_flash_read(fs, fffs_sector_metadata_offset(fs, sector),
            tail, sizeof(tail));
    if (err != FFFS_OK) {
        return err;
    }

    const uint8_t *md = tail;
    const uint8_t *footer = tail + FFFS_MD_SIZE;
    if (footer[4] != FFFS_SECTOR_TYPE_MIXED ||
            footer[5] != FFFS_SECTOR_FLAGS_VALID ||
            footer[6] != 0xff || footer[7] != 0xff ||
            memcmp(footer + 8, FFFS_SECTOR_MAGIC, 4) != 0) {
        return FFFS_ERR_CORRUPT;
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
    uint16_t next_head = load16(md + 12);
    if (next_head != 0 &&
            (next_head < fs->index_sectors || next_head >= fs->sector_count)) {
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
    if (next) {
        *next = next_head;
    }
    if (st) {
        size_t name_len = md[1];
        memcpy(st->name, md + 14, name_len);
        st->name[name_len] = '\0';
        st->size = load32(md + 8);
    }
    return FFFS_OK;
}

int fffs_write_extent_metadata(struct fffs_file *file, uint16_t sector,
        uint32_t serial, uint16_t data_len, uint32_t total_size,
        uint16_t next, bool commit_index) {
    struct fffs *fs = file->fs;
    uint8_t tail[FFFS_MD_SIZE + FFFS_SECTOR_FOOTER_SIZE];
    uint8_t *md = tail;
    uint8_t *footer = tail + FFFS_MD_SIZE;
    memset(tail, 0xff, sizeof(tail));
    md[0] = FFFS_MD_FLAGS_VALID;
    md[1] = (uint8_t)strlen(file->name);
    store16(md + 2, file->slot);
    store16(md + 4, 0);
    store16(md + 6, data_len);
    store32(md + 8, total_size);
    store16(md + 12, next);
    memcpy(md + 14, file->name, md[1]);
    md[63] = FFFS_MD_TYPE_BASELINE;
    encode_sector_footer(footer, serial);

    int err = fffs_flash_program_aligned(fs,
            fffs_sector_metadata_offset(fs, sector), tail, sizeof(tail));
    if (err != FFFS_OK) {
        return err;
    }
    return commit_index ? fffs_append_index_record(fs, file->slot, file->head)
        : FFFS_OK;
}
