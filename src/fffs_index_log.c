/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS circular namespace journal: slot helpers, index-sector discovery,
 * replay, compaction, and rotation.
 */

#include "fffs_internal.h"

#include <string.h>

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
        0xff,
    };
    int err = fffs_backend_program_aligned(backend, offset, hdr, sizeof(hdr));
    if (err != FFFS_OK) {
        return err;
    }
    uint8_t flags = FFFS_INDEX_FLAGS_VALID;
    return fffs_backend_program_aligned(backend, offset + 7u,
            &flags, sizeof(flags));
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
    *slot = fffs_load_le16(rec);
    *head = fffs_load_le16(rec + 2);
    return FFFS_OK;
}

static size_t logical_index_sector(const struct fffs *fs, size_t seq_pos) {
    return (fs->oldest_index_sector + seq_pos) % fs->index_sectors;
}

static size_t index_sector_begin(const struct fffs *fs, size_t sector) {
    return sector * fs->sector_size + FFFS_HEADER_SIZE;
}

static size_t index_sector_end(const struct fffs *fs, size_t sector) {
    if (sector == fs->active_index_sector) {
        return fs->next_index_offset;
    }
    return (sector + 1) * fs->sector_size;
}

static int program_index_record_at(struct fffs *fs, size_t offset,
        uint16_t slot, uint16_t head) {
    uint8_t slot_bytes[2];
    fffs_store_le16(slot_bytes, slot);
    int err = fffs_flash_program_aligned(fs, offset, slot_bytes,
            sizeof(slot_bytes));
    if (err != FFFS_OK) {
        return err;
    }

    uint8_t head_bytes[2];
    fffs_store_le16(head_bytes, head);
    return fffs_flash_program_aligned(fs, offset + 2u, head_bytes,
            sizeof(head_bytes));
}

static int clobber_active_tail(struct fffs *fs, size_t rec_off) {
    uint8_t rec[4] = {0};
    int err = fffs_flash_program_aligned(fs, rec_off, rec, sizeof(rec));
    if (err != FFFS_OK) {
        return err;
    }
    fs->next_index_offset = rec_off + 4;
    return FFFS_OK;
}

static int clobber_or_reject_active_tail(struct fffs *fs, size_t rec_off,
        bool terminal_active_tail) {
    if (fs->strict || !terminal_active_tail) {
        return FFFS_ERR_CORRUPT;
    }
    return clobber_active_tail(fs, rec_off);
}

static int index_flash_span_erased(struct fffs *fs, size_t offset,
        size_t size, bool *erased) {
    uint8_t buf[32];
    *erased = true;
    while (size > 0) {
        size_t n = size < sizeof(buf) ? size : sizeof(buf);
        int err = fffs_flash_read(fs, offset, buf, n);
        if (err != FFFS_OK) {
            return err;
        }
        if (!fffs_flash_bytes_erased(buf, n)) {
            *erased = false;
            return FFFS_OK;
        }
        offset += n;
        size -= n;
    }
    return FFFS_OK;
}

static int active_index_record_is_terminal(struct fffs *fs, size_t rec_off,
        const uint8_t *chunk, size_t pos, size_t nread, size_t end,
        bool *terminal) {
    *terminal = rec_off + 4 == end;
    if (*terminal) {
        return FFFS_OK;
    }

    uint8_t next_rec[4];
    if (pos + 8 <= nread) {
        memcpy(next_rec, chunk + pos + 4, sizeof(next_rec));
    } else {
        int err = fffs_flash_read(fs, rec_off + 4, next_rec,
                sizeof(next_rec));
        if (err != FFFS_OK) {
            return err;
        }
    }
    if (!fffs_flash_bytes_erased(next_rec, sizeof(next_rec))) {
        return FFFS_OK;
    }

    size_t rest_off = rec_off + 8;
    size_t chunk_end = rec_off + (nread - pos);
    if (rest_off < chunk_end) {
        if (!fffs_flash_bytes_erased(chunk + pos + 8, chunk_end - rest_off)) {
            return FFFS_OK;
        }
        rest_off = chunk_end;
    }
    if (rest_off >= end) {
        *terminal = true;
        return FFFS_OK;
    }
    return index_flash_span_erased(fs, rest_off, end - rest_off, terminal);
}

static int tombstone_index_sector(struct fffs *fs, size_t sector) {
    uint8_t flags = (uint8_t)(FFFS_INDEX_FLAGS_VALID &
        (uint8_t)~FFFS_INDEX_FLAG_TOMBSTONED);
    return fffs_flash_program_aligned(fs, sector * fs->sector_size + 7,
            &flags, sizeof(flags));
}

#if FFFS_INDEX_COMPACT_OUTER_SCAN_SIZE < 4u || \
    FFFS_INDEX_COMPACT_OUTER_SCAN_SIZE % 4u != 0
#error "FFFS_INDEX_COMPACT_OUTER_SCAN_SIZE must be a multiple of 4 and at least 4"
#endif

#if FFFS_INDEX_COMPACT_WRITE_BUFFER_SIZE < 4u || \
    FFFS_INDEX_COMPACT_WRITE_BUFFER_SIZE % 4u != 0
#error "FFFS_INDEX_COMPACT_WRITE_BUFFER_SIZE must be a multiple of 4 and at least 4"
#endif

struct index_record_reader {
    uint8_t window[FFFS_INDEX_COMPACT_OUTER_SCAN_SIZE];
    size_t window_start;
    size_t window_len;
};

static int index_record_reader_load(struct fffs *fs,
        struct index_record_reader *reader, size_t offset) {
    if (offset >= reader->window_start &&
            offset + 4 <= reader->window_start + reader->window_len) {
        return FFFS_OK;
    }

    size_t sector = offset / fs->sector_size;
    size_t begin = sector * fs->sector_size + FFFS_HEADER_SIZE;
    size_t end = index_sector_end(fs, sector);
    size_t rel = offset - begin;
    size_t base = begin + rel - (rel % sizeof(reader->window));
    size_t nread = end - base;
    if (nread > sizeof(reader->window)) {
        nread = sizeof(reader->window);
    }
    nread -= nread % 4;
    if (nread < 4) {
        return FFFS_ERR_CORRUPT;
    }

    int err = fffs_flash_read(fs, base, reader->window, nread);
    if (err != FFFS_OK) {
        return err;
    }
    reader->window_start = base;
    reader->window_len = nread;
    return FFFS_OK;
}

static int read_compact_source_record(struct fffs *fs,
        struct index_record_reader *reader, size_t offset,
        uint16_t *slot, uint16_t *head, bool *erased) {
    int err = index_record_reader_load(fs, reader, offset);
    if (err != FFFS_OK) {
        return err;
    }
    const uint8_t *rec = reader->window + (offset - reader->window_start);
    *slot = fffs_load_le16(rec);
    *head = fffs_load_le16(rec + 2);
    *erased = *slot == UINT16_MAX && *head == UINT16_MAX;
    return FFFS_OK;
}

struct index_record_writer {
    uint8_t buf[FFFS_INDEX_COMPACT_WRITE_BUFFER_SIZE];
    size_t len;
    bool pending_rotation;
};

static int rotate_index_begin(struct fffs *fs);
static int rotate_index_commit(struct fffs *fs);

static int index_record_writer_prepare_spill(struct fffs *fs,
        struct index_record_writer *writer) {
    if (writer->pending_rotation || fs->index_sequence_count >=
            fs->index_sectors) {
        return FFFS_ERR_NO_SPACE;
    }

    int err = rotate_index_begin(fs);
    if (err != FFFS_OK) {
        return err;
    }

    writer->pending_rotation = true;
    return FFFS_OK;
}

static int index_record_writer_commit_spill(struct fffs *fs,
        struct index_record_writer *writer) {
    if (!writer->pending_rotation) {
        return FFFS_OK;
    }
    int err = rotate_index_commit(fs);
    if (err != FFFS_OK) {
        return err;
    }
    writer->pending_rotation = false;
    return FFFS_OK;
}

static int index_record_writer_flush(struct fffs *fs,
        struct index_record_writer *writer) {
    if (writer->len == 0) {
        return FFFS_OK;
    }
    int err = fffs_flash_program_aligned(fs, fs->next_index_offset,
            writer->buf, writer->len);
    if (err != FFFS_OK) {
        return err;
    }
    fs->next_index_offset += writer->len;
    writer->len = 0;
    return FFFS_OK;
}

static int index_record_writer_append(struct fffs *fs,
        struct index_record_writer *writer, uint16_t slot, uint16_t head) {
    int err = index_record_writer_flush(fs, writer);
    if (err != FFFS_OK) {
        return err;
    }

    if (fs->next_index_offset + 4 >
            (fs->active_index_sector + 1) * fs->sector_size) {
        err = index_record_writer_prepare_spill(fs, writer);
        if (err != FFFS_OK) {
            return err;
        }
    }

    err = program_index_record_at(fs, fs->next_index_offset, slot, head);
    if (err != FFFS_OK) {
        return err;
    }
    fs->next_index_offset += 4;
    return FFFS_OK;
}

static int index_compact_oldest_copy(struct fffs *fs, size_t *source_out) {
    if (fs->index_sequence_count <= 1) {
        return FFFS_OK;
    }

    size_t source_seq_pos = 0;
    size_t source_sector = fs->oldest_index_sector;
    if (source_out) {
        *source_out = source_sector;
    }
    size_t source_begin = index_sector_begin(fs, source_sector);
    size_t source_end = index_sector_end(fs, source_sector);
    struct index_record_reader reader = {0};
    struct index_record_writer writer = {0};

    for (size_t off = source_begin; off + 4 <= source_end; off += 4) {
        uint16_t slot;
        uint16_t head;
        bool erased;
        int err = read_compact_source_record(fs, &reader, off, &slot,
                &head, &erased);
        if (err != FFFS_OK) {
            return err;
        }
        if (erased) {
            break; //basically EOF for the index sector
        }
        if ((slot == 0 && head == 0) || head == 0) {
            continue; //clobbered partial / tombstoned
        }

        bool current;
        err = fffs_index_record_is_current(fs, source_seq_pos, off, slot,
                head, &current);
        if (err != FFFS_OK) {
            return err;
        }
        if (!current) {
            continue;
        }

        err = index_record_writer_append(fs, &writer, slot, head);
        if (err != FFFS_OK) {
            return err;
        }
    }

    int err = index_record_writer_flush(fs, &writer);
    if (err != FFFS_OK) {
        return err;
    }

    err = index_record_writer_commit_spill(fs, &writer);
    if (err != FFFS_OK) {
        return err;
    }

    return FFFS_OK;
}

static int index_compact_oldest_finish(struct fffs *fs,
        size_t source_sector) {
    int err = tombstone_index_sector(fs, source_sector);
    if (err != FFFS_OK) {
        return err;
    }

    fs->oldest_index_sector = logical_index_sector(fs, 1);
    fs->index_sequence_count--;
    return FFFS_OK;
}

int fffs_index_compact_oldest(struct fffs *fs) {
    size_t source_sector = 0;
    int err = index_compact_oldest_copy(fs, &source_sector);
    if (err != FFFS_OK || fs->index_sequence_count <= 1) {
        return err;
    }
    return index_compact_oldest_finish(fs, source_sector);
}

int fffs_index_finish_interrupted_compaction(struct fffs *fs) {
    if (fs->index_sequence_count < fs->index_sectors) {
        return FFFS_OK;
    }
    return index_compact_oldest_finish(fs, fs->oldest_index_sector);
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

    int err = program_index_record_at(fs, fs->next_index_offset, slot, head);
    if (err != FFFS_OK) {
        return err;
    }
    fs->next_index_offset += 4;
    return fffs_index_set(fs, slot, head);
}

static int rotate_index_begin(struct fffs *fs) {
    size_t new_active = fs->active_index_sector + 1;
    if (new_active >= fs->index_sectors) {
        new_active = 0;
    }

    size_t new_base = new_active * fs->sector_size;
    int err = fffs_map_backend_status(fs->backend.erase(fs->backend.ctx,
                new_base, fs->sector_size));
#if FFFS_PROFILE_TRACE
    fffs_profile_flash(fs, FFFS_PROFILE_FLASH_ERASE, new_base,
            fs->sector_size);
#endif
    if (err != FFFS_OK) {
        return err;
    }

    uint8_t serial = (uint8_t)((fs->active_index_serial + 1) & 0x0f);
    fs->active_index_sector = new_active;
    fs->active_index_serial = serial;
    fs->next_index_offset = new_base + FFFS_HEADER_SIZE;
    if (fs->index_sequence_count < fs->index_sectors) {
        fs->index_sequence_count++;
    }
    return FFFS_OK;
}

static int rotate_index_commit(struct fffs *fs) {
    size_t base = fs->active_index_sector * fs->sector_size;
    return fffs_program_index_header(&fs->backend, base, fs->index_sectors,
            fs->sector_shift, fs->active_index_serial);
}

int fffs_rotate_index(struct fffs *fs) {
    int err = rotate_index_begin(fs);
    if (err != FFFS_OK) {
        return err;
    }

    if (fs->index_sequence_count == fs->index_sectors) {
        size_t source_sector;
        err = index_compact_oldest_copy(fs, &source_sector);
        if (err != FFFS_OK) {
            return err;
        }
        err = rotate_index_commit(fs);
        if (err != FFFS_OK) {
            return err;
        }
        return index_compact_oldest_finish(fs, source_sector);
    }

    return rotate_index_commit(fs);
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

        err = fffs_read_sector_serial(fs, (uint16_t)sector, &serial);
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
    uint8_t *chunk = fs->scratch;
    size_t chunk_size = fs->scratch_size;

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
            FFFS_PROFILE_PUSH(fs, FFFS_PROFILE_INDEX_REPLAY);
            int err = fffs_flash_read(fs, off, chunk, nread);
            FFFS_PROFILE_POP(fs, FFFS_PROFILE_INDEX_REPLAY);
            if (err != FFFS_OK) {
                return err;
            }
            fffs_scratch_bump(fs);
            for (size_t pos = 0; pos < nread; pos += 4) {
                uint16_t slot = fffs_load_le16(chunk + pos);
                uint16_t head = fffs_load_le16(chunk + pos + 2);
                size_t rec_off = off + pos;

                if (slot == UINT16_MAX && head == UINT16_MAX) {
                    if (active) {
                        fs->next_index_offset = rec_off;
                    }
                    goto next_sector;
                }
                if (slot == 0 && head == 0) {
                    if (active) {
                        fs->next_index_offset = rec_off + 4;
                    }
                    continue;
                }
                bool terminal_active_tail = false;
                /*
                 * An interrupted final append can leave wrong slot/head bits.
                 * Only a terminal active record is eligible for recovery.
                 */
                if (active) {
                    err = active_index_record_is_terminal(fs, rec_off, chunk,
                            pos, nread, end, &terminal_active_tail);
                    if (err != FFFS_OK) {
                        return err;
                    }
                }

                if (slot == 0 || slot == UINT16_MAX ||
                        head == UINT16_MAX) {
                    err = clobber_or_reject_active_tail(fs, rec_off,
                            terminal_active_tail);
                    if (err != FFFS_OK) {
                        return err;
                    }
                    goto next_sector;
                }
                if (head == 0) {
                    err = fffs_index_remove(fs, slot);
                    if (err != FFFS_OK) {
                        return err;
                    }
                    continue;
                }

                if (head < fs->index_sectors || head >= fs->sector_count) {
                    err = clobber_or_reject_active_tail(fs, rec_off,
                            terminal_active_tail);
                    if (err != FFFS_OK) {
                        return err;
                    }
                    goto next_sector;
                }

                if (terminal_active_tail) {
                    uint32_t root_size;
                    err = fffs_read_file_root_md(fs, head, slot, NULL,
                            NULL, NULL, NULL, NULL, &root_size, NULL);
                    if (err != FFFS_OK) {
                        err = clobber_or_reject_active_tail(fs, rec_off,
                                terminal_active_tail);
                        if (err != FFFS_OK) {
                            return err;
                        }
                        goto next_sector;
                    }
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
