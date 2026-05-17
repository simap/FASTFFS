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
        backend->program_granule <= FFFS_FILE_CACHE_SIZE &&
        backend->size % backend->program_granule == 0 &&
        backend->program_granule % backend->read_granule == 0 &&
        backend->read && backend->program && backend->erase;
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
    int err = backend_program_aligned(&fs->backend, offset, buffer, size);
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
    uint8_t rec[4];
    store16(rec, slot);
    store16(rec + 2, head);
    return fffs_flash_program_aligned(fs, offset, rec, sizeof(rec));
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
    *slot = load16(rec);
    *head = load16(rec + 2);
    *erased = *slot == UINT16_MAX && *head == UINT16_MAX;
    if (*erased) {
        return FFFS_OK;
    }
    if (*slot == 0 && *head == 0) {
        return FFFS_OK;
    }
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
    if (fs->next_index_offset + writer->len + 4 >
            (fs->active_index_sector + 1) * fs->sector_size) {
        int err = index_record_writer_flush(fs, writer);
        if (err != FFFS_OK) {
            return err;
        }
        err = index_record_writer_prepare_spill(fs, writer);
        if (err != FFFS_OK) {
            return err;
        }
    }

    if (writer->len + 4 > sizeof(writer->buf)) {
        int err = index_record_writer_flush(fs, writer);
        if (err != FFFS_OK) {
            return err;
        }
    }

    store16(writer->buf + writer->len, slot);
    store16(writer->buf + writer->len + 2, head);
    writer->len += 4;
    if (writer->len == sizeof(writer->buf)) {
        return index_record_writer_flush(fs, writer);
    }
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
            continue; //clobbered / tombstoned
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
    size_t raw = fs->sector_size - FFFS_SECTOR_FOOTER_SIZE -
        FFFS_MD_FILE_RECORD_SIZE;
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
    return fffs_sector_footer_offset(fs, sector) - FFFS_MD_FILE_RECORD_SIZE;
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
    enum fffs_lifecycle_object_state footer_state =
        fffs_lifecycle_decode_footer(footer[5]);
    if (footer[4] != FFFS_SECTOR_TYPE_FILE ||
            footer[6] != 0xff || footer[7] != 0xff ||
            memcmp(footer + 8, FFFS_SECTOR_MAGIC, 4) != 0 ||
            (footer_state != FFFS_LIFECYCLE_OBJECT_LIVE &&
             footer_state != FFFS_LIFECYCLE_OBJECT_TOMBSTONED)) {
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
    footer[4] = FFFS_SECTOR_TYPE_FILE;
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
        FFFS_SECTOR_TYPE_FILE,
        FFFS_SECTOR_FLAGS_TOMBSTONED,
        0xff,
        0xff,
    };
    return fffs_flash_program_aligned(fs,
            fffs_sector_footer_offset(fs, sector) + 4, state, sizeof(state));
}

int fffs_mark_sector_full(struct fffs *fs, uint16_t sector) {
    uint8_t state = FFFS_SECTOR_FLAGS_FULL;
    return fffs_flash_program_aligned(fs,
            fffs_sector_footer_offset(fs, sector) + 5, &state, sizeof(state));
}

struct decoded_file_md {
    uint8_t type;
    uint8_t state;
    uint16_t slot;
    uint16_t data_off;
    uint16_t data_len;
    uint16_t next;
    uint16_t span_len;
    uint32_t size;
    uint32_t file_offset;
    size_t record_start;
    size_t record_len;
};

enum decoded_file_md_state {
    FFFS_DECODED_MD_ERASED,
    FFFS_DECODED_MD_INVALID,
    FFFS_DECODED_MD_LIVE,
    FFFS_DECODED_MD_PARTIAL_TOMBSTONE,
    FFFS_DECODED_MD_TOMBSTONED,
};

static bool erased_bytes(const uint8_t *p, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (p[i] != 0xff) {
            return false;
        }
    }
    return true;
}

static enum decoded_file_md_state decode_file_md_lifecycle(uint8_t state) {
    enum fffs_bitmirror_state valid = fffs_lifecycle_valid_pair(state);
    enum fffs_bitmirror_state tombstone =
        fffs_lifecycle_tombstone_pair(state);
    if (valid != FFFS_BITMIRROR_CLEARED) {
        return FFFS_DECODED_MD_INVALID;
    }
    if (tombstone == FFFS_BITMIRROR_SET) {
        return FFFS_DECODED_MD_LIVE;
    }
    if (tombstone == FFFS_BITMIRROR_CLEARED) {
        return FFFS_DECODED_MD_TOMBSTONED;
    }
    return FFFS_DECODED_MD_PARTIAL_TOMBSTONE;
}

static int read_root_prefix(struct fffs *fs, uint16_t sector,
        const struct decoded_file_md *md, struct fffs_stat *st,
        uint16_t *payload_data_off, uint16_t *payload_data_len,
        struct fffs_read_cache_view *cache) {
    if (md->data_len < 2) {
        return FFFS_ERR_CORRUPT;
    }
    uint8_t local[FFFS_MAX_NAME + 2u];
    bool use_cache = cache && cache->data && cache->capacity >= sizeof(local);
    uint8_t *buf = use_cache ? cache->data : local;
    size_t buf_size = use_cache ? cache->capacity : sizeof(local);
    size_t nread = md->data_len < buf_size ? md->data_len : buf_size;
    int err = fffs_flash_read(fs, (size_t)sector * fs->sector_size +
            md->data_off, buf, nread);
    if (err != FFFS_OK) {
        return err;
    }
    size_t name_len = buf[0];
    size_t vmeta_len_off = 1u + name_len;
    if (name_len == 0 || name_len > FFFS_MAX_NAME ||
            vmeta_len_off >= md->data_len || vmeta_len_off >= nread) {
        return FFFS_ERR_CORRUPT;
    }

    size_t vmeta_len = buf[vmeta_len_off];
    size_t payload_off = 1u + name_len + 1u + vmeta_len;
    if (payload_off > md->data_len) {
        return FFFS_ERR_CORRUPT;
    }

    if (st) {
        memcpy(st->name, buf + 1, name_len);
        st->name[name_len] = '\0';
        st->size = md->size;
    }
    if (payload_data_off) {
        *payload_data_off = (uint16_t)(md->data_off + payload_off);
    }
    if (payload_data_len) {
        *payload_data_len = (uint16_t)(md->data_len - payload_off);
    }
    if (use_cache) {
        size_t cached_payload = payload_off < nread ? nread - payload_off : 0;
        if (cached_payload != 0) {
            memmove(cache->data, cache->data + payload_off, cached_payload);
        }
        cache->len = cached_payload;
        cache->data_pos = 0;
    }
    return FFFS_OK;
}

#if FFFS_MD_READ_WINDOW_SIZE < FFFS_MD_FILE_RECORD_SIZE + FFFS_SECTOR_FOOTER_SIZE
#error "FFFS_MD_READ_WINDOW_SIZE must fit at least one metadata record and footer"
#endif

struct file_md_reader {
    uint8_t window[FFFS_MD_READ_WINDOW_SIZE];
    uint16_t sector;
    size_t window_start;
    size_t window_len;
};

static int file_md_reader_load(struct fffs *fs,
        struct file_md_reader *reader, size_t offset, size_t size) {
    if (offset >= reader->window_start &&
            offset + size <= reader->window_start + reader->window_len) {
        return FFFS_OK;
    }

    size_t sector_base = (size_t)reader->sector * fs->sector_size;
    size_t sector_end = sector_base + fs->sector_size;
    size_t capacity = sizeof(reader->window);
    if (size > capacity || offset < sector_base || offset + size > sector_end) {
        return FFFS_ERR_CORRUPT;
    }

    size_t end = offset + size;
    size_t base = sector_base;
    if (end > sector_base + capacity) {
        base = end - capacity;
    }
    size_t nread = sector_end - base;
    if (nread > capacity) {
        nread = capacity;
    }
    int err = fffs_flash_read(fs, base, reader->window, nread);
    if (err != FFFS_OK) {
        return err;
    }
    reader->window_start = base;
    reader->window_len = nread;
    return FFFS_OK;
}

static int file_md_reader_read(struct fffs *fs,
        struct file_md_reader *reader, size_t offset, void *out, size_t size) {
    int err = file_md_reader_load(fs, reader, offset, size);
    if (err != FFFS_OK) {
        return err;
    }
    memcpy(out, reader->window + (offset - reader->window_start), size);
    return FFFS_OK;
}

static int file_md_reader_view(struct fffs *fs,
        struct file_md_reader *reader, size_t offset, size_t size,
        const uint8_t **out) {
    int err = file_md_reader_load(fs, reader, offset, size);
    if (err != FFFS_OK) {
        return err;
    }
    *out = reader->window + (offset - reader->window_start);
    return FFFS_OK;
}

static bool validate_live_file_footer(const uint8_t *footer) {
    enum fffs_lifecycle_object_state footer_state =
        fffs_lifecycle_decode_footer(footer[5]);
    return footer[4] == FFFS_SECTOR_TYPE_FILE &&
        footer[6] == 0xff && footer[7] == 0xff &&
        memcmp(footer + 8, FFFS_SECTOR_MAGIC, 4) == 0 &&
        footer_state == FFFS_LIFECYCLE_OBJECT_LIVE;
}

static int decode_file_md_record(struct fffs *fs,
        struct file_md_reader *reader, size_t cursor,
        struct decoded_file_md *out, enum decoded_file_md_state *state) {
    *state = FFFS_DECODED_MD_INVALID;
    uint8_t type;
    int err = file_md_reader_read(fs, reader,
            (size_t)reader->sector * fs->sector_size + cursor - 1u,
            &type, sizeof(type));
    if (err != FFFS_OK) {
        return err;
    }
    if (type == 0xff) {
        *state = FFFS_DECODED_MD_ERASED;
        return FFFS_OK;
    }

    size_t record_len = FFFS_MD_FILE_RECORD_SIZE;
    if (cursor < FFFS_SECTOR_FOOTER_SIZE + record_len) {
        return FFFS_ERR_CORRUPT;
    }
    size_t record_start = cursor - record_len;
    memset(out, 0, sizeof(*out));
    out->type = type;
    out->record_start = record_start;
    out->record_len = record_len;

    if (type == FFFS_MD_TYPE_FILE_ROOT_V1 ||
            type == FFFS_MD_TYPE_FILE_CONT_V1) {
        /* Current file metadata records have one fixed size. */
    } else {
        return FFFS_OK;
    }

    uint8_t buf[FFFS_MD_FILE_RECORD_SIZE];
    err = file_md_reader_read(fs, reader,
            (size_t)reader->sector * fs->sector_size + record_start,
            buf, record_len);
    if (err != FFFS_OK) {
        return err;
    }
    if (erased_bytes(buf, record_len)) {
        *state = FFFS_DECODED_MD_ERASED;
        return FFFS_OK;
    }

    out->state = buf[0];
    *state = decode_file_md_lifecycle(buf[0]);
    if (*state == FFFS_DECODED_MD_INVALID ||
            *state == FFFS_DECODED_MD_ERASED) {
        return FFFS_OK;
    }
    out->slot = load16(buf + 1);
    out->next = load16(buf + 3);
    out->span_len = load16(buf + 5);
    out->data_off = load16(buf + 7);
    out->data_len = load16(buf + 9);
    if (out->span_len == 0 ||
            (size_t)out->data_off + out->data_len > record_start) {
        *state = FFFS_DECODED_MD_INVALID;
        return FFFS_OK;
    }
    if (out->next != 0 &&
            (out->next < fs->index_sectors || out->next >= fs->sector_count)) {
        *state = FFFS_DECODED_MD_INVALID;
        return FFFS_OK;
    }
    if (type == FFFS_MD_TYPE_FILE_ROOT_V1) {
        out->size = load32(buf + 11);
    } else {
        out->file_offset = load32(buf + 11);
    }
    return FFFS_OK;
}

static int read_metadata_for_slot_impl(struct fffs *fs, uint16_t sector,
        uint16_t want_slot, struct fffs_stat *st,
        uint16_t *data_off, uint16_t *data_len,
        uint16_t *next, struct fffs_read_cache_view *cache) {
    if (sector < fs->index_sectors || sector >= fs->sector_count) {
        return FFFS_ERR_CORRUPT;
    }

    struct file_md_reader reader = {
        .sector = sector,
    };
    const uint8_t *footer;
    FFFS_PROFILE_PUSH(fs, FFFS_PROFILE_READ_METADATA);
    int err = file_md_reader_view(fs, &reader,
            fffs_sector_footer_offset(fs, sector), FFFS_SECTOR_FOOTER_SIZE,
            &footer);
    FFFS_PROFILE_POP(fs, FFFS_PROFILE_READ_METADATA);
    if (err != FFFS_OK) {
        return err;
    }
    if (!validate_live_file_footer(footer)) {
        return FFFS_ERR_CORRUPT;
    }

    size_t claimed_data_end = 0;
    size_t cursor = fs->sector_size - FFFS_SECTOR_FOOTER_SIZE;
    while (cursor > FFFS_SECTOR_FOOTER_SIZE) {
        struct decoded_file_md md;
        enum decoded_file_md_state md_state;
        err = decode_file_md_record(fs, &reader, cursor, &md, &md_state);
        if (err != FFFS_OK) {
            return err;
        }
        if (md_state == FFFS_DECODED_MD_ERASED) {
            break;
        }
        if (md_state == FFFS_DECODED_MD_INVALID) {
            break;
        }
        cursor = md.record_start;
        size_t data_end = (size_t)md.data_off + md.data_len;
        if (data_end > claimed_data_end) {
            claimed_data_end = data_end;
        }

        if (md_state == FFFS_DECODED_MD_TOMBSTONED ||
                md_state == FFFS_DECODED_MD_PARTIAL_TOMBSTONE) {
            if (cursor <= claimed_data_end) {
                break;
            }
            continue;
        }
        if (md.slot != want_slot) {
            if (cursor <= claimed_data_end) {
                break;
            }
            continue;
        }

        uint16_t payload_data_off = md.data_off;
        uint16_t payload_data_len = md.data_len;
        if (md.type == FFFS_MD_TYPE_FILE_ROOT_V1 &&
                (st || data_off || data_len || cache)) {
            err = read_root_prefix(fs, sector, &md, st,
                    &payload_data_off, &payload_data_len, cache);
            if (err != FFFS_OK) {
                return err;
            }
        } else if (st) {
            memset(st, 0, sizeof(*st));
        }

        if (data_off) {
            *data_off = payload_data_off;
        }
        if (data_len) {
            *data_len = payload_data_len;
        }
        if (next) {
            *next = md.next;
        }
        return FFFS_OK;
    }
    return FFFS_ERR_CORRUPT;
}

int fffs_tombstone_metadata_for_slot(struct fffs *fs, uint16_t sector,
        uint16_t want_slot) {
    if (sector < fs->index_sectors || sector >= fs->sector_count) {
        return FFFS_ERR_CORRUPT;
    }

    uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
    int err = fffs_flash_read(fs, fffs_sector_footer_offset(fs, sector),
            footer, sizeof(footer));
    if (err != FFFS_OK) {
        return err;
    }
    enum fffs_lifecycle_object_state footer_state =
        fffs_lifecycle_decode_footer(footer[5]);
    if (footer[4] != FFFS_SECTOR_TYPE_FILE ||
            footer[6] != 0xff || footer[7] != 0xff ||
            memcmp(footer + 8, FFFS_SECTOR_MAGIC, 4) != 0 ||
            footer_state != FFFS_LIFECYCLE_OBJECT_LIVE) {
        return FFFS_ERR_CORRUPT;
    }

    size_t claimed_data_end = 0;
    size_t cursor = fs->sector_size - FFFS_SECTOR_FOOTER_SIZE;
    struct file_md_reader reader = {
        .sector = sector,
    };
    while (cursor > FFFS_SECTOR_FOOTER_SIZE) {
        struct decoded_file_md md;
        enum decoded_file_md_state md_state;
        err = decode_file_md_record(fs, &reader, cursor, &md, &md_state);
        if (err != FFFS_OK) {
            return err;
        }
        if (md_state == FFFS_DECODED_MD_ERASED) {
            return FFFS_OK;
        }
        if (md_state == FFFS_DECODED_MD_INVALID) {
            return FFFS_ERR_CORRUPT;
        }
        cursor = md.record_start;
        size_t data_end = (size_t)md.data_off + md.data_len;
        if (data_end > claimed_data_end) {
            claimed_data_end = data_end;
        }
        if (md_state != FFFS_DECODED_MD_LIVE || md.slot != want_slot) {
            if (cursor <= claimed_data_end) {
                break;
            }
            continue;
        }

        uint8_t tombstone = FFFS_MD_FLAGS_TOMBSTONED;
        return fffs_flash_program_aligned(fs,
                (size_t)sector * fs->sector_size + md.record_start,
                &tombstone, sizeof(tombstone));
    }
    return FFFS_OK;
}

int fffs_visit_metadata_records(struct fffs *fs, uint16_t sector,
        fffs_md_record_visitor visitor, void *ctx) {
    if (!visitor || sector < fs->index_sectors || sector >= fs->sector_count) {
        return FFFS_ERR_INVALID;
    }

    uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
    int err = fffs_flash_read(fs, fffs_sector_footer_offset(fs, sector),
            footer, sizeof(footer));
    if (err != FFFS_OK) {
        return err;
    }
    enum fffs_lifecycle_object_state footer_state =
        fffs_lifecycle_decode_footer(footer[5]);
    if (footer[4] != FFFS_SECTOR_TYPE_FILE ||
            footer[6] != 0xff || footer[7] != 0xff ||
            memcmp(footer + 8, FFFS_SECTOR_MAGIC, 4) != 0 ||
            footer_state != FFFS_LIFECYCLE_OBJECT_LIVE) {
        return FFFS_ERR_CORRUPT;
    }

    size_t cursor = fs->sector_size - FFFS_SECTOR_FOOTER_SIZE;
    size_t claimed_data_end = 0;
    struct file_md_reader reader = {
        .sector = sector,
    };
    while (cursor > FFFS_SECTOR_FOOTER_SIZE) {
        struct decoded_file_md md;
        enum decoded_file_md_state md_state;
        err = decode_file_md_record(fs, &reader, cursor, &md, &md_state);
        if (err != FFFS_OK) {
            return err;
        }
        if (md_state == FFFS_DECODED_MD_ERASED ||
                md_state == FFFS_DECODED_MD_INVALID) {
            return FFFS_OK;
        }
        cursor = md.record_start;
        size_t data_end = (size_t)md.data_off + md.data_len;
        if (data_end > claimed_data_end) {
            claimed_data_end = data_end;
        }

        struct fffs_md_record record = {
            .type = md.type,
            .state = md.state,
            .slot = md.slot,
            .next = md.next,
            .data_off = md.data_off,
            .data_len = md.data_len,
            .record_start = md.record_start,
            .record_len = md.record_len,
        };
        err = visitor(fs, &record, ctx);
        if (err != FFFS_OK) {
            return err;
        }
        if (cursor <= claimed_data_end) {
            break;
        }
    }
    return FFFS_OK;
}

int fffs_read_metadata_record_at(struct fffs *fs, uint16_t sector,
        size_t cursor, size_t *claimed_data_end,
        struct fffs_md_record *record, size_t *next_cursor,
        bool *erased, bool *invalid, bool *done) {
    if (sector < fs->index_sectors || sector >= fs->sector_count ||
            !claimed_data_end || !record || !next_cursor ||
            !erased || !invalid || !done) {
        return FFFS_ERR_INVALID;
    }

    struct decoded_file_md md;
    enum decoded_file_md_state md_state;
    struct file_md_reader reader = {
        .sector = sector,
    };
    int err = decode_file_md_record(fs, &reader, cursor, &md, &md_state);
    if (err != FFFS_OK) {
        return err;
    }
    *erased = md_state == FFFS_DECODED_MD_ERASED;
    *invalid = md_state == FFFS_DECODED_MD_INVALID;
    *done = *erased || *invalid;
    if (*erased) {
        *next_cursor = cursor;
        memset(record, 0, sizeof(*record));
        return FFFS_OK;
    }
    *next_cursor = md.record_start;
    *record = (struct fffs_md_record){
        .type = md.type,
        .state = md.state,
        .slot = md.slot,
        .next = md.next,
        .data_off = md.data_off,
        .data_len = md.data_len,
        .record_start = md.record_start,
        .record_len = md.record_len,
    };
    if (*invalid) {
        return FFFS_OK;
    }

    size_t data_end = (size_t)md.data_off + md.data_len;
    if (data_end > *claimed_data_end) {
        *claimed_data_end = data_end;
    }
    *next_cursor = md.record_start;
    *done = md.record_start <= *claimed_data_end;
    return FFFS_OK;
}

int fffs_read_metadata_for_slot(struct fffs *fs, uint16_t sector,
        uint16_t want_slot, struct fffs_stat *st, uint16_t *data_off,
        uint16_t *data_len, uint16_t *next,
        struct fffs_read_cache_view *cache) {
    return read_metadata_for_slot_impl(fs, sector, want_slot, st,
            data_off, data_len, next, cache);
}

int fffs_find_sector_free_window(struct fffs *fs, uint16_t sector,
        uint16_t min_free, uint16_t reject_slot, uint16_t *data_off,
        uint16_t *record_off, bool *needs_footer) {
    if (sector < fs->index_sectors || sector >= fs->sector_count ||
            !data_off || !record_off || !needs_footer) {
        return FFFS_ERR_INVALID;
    }

    uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
    int err = fffs_flash_read(fs, fffs_sector_footer_offset(fs, sector),
            footer, sizeof(footer));
    if (err != FFFS_OK) {
        return err;
    }

    size_t footer_off = fs->sector_size - FFFS_SECTOR_FOOTER_SIZE;
    if (erased_bytes(footer, sizeof(footer))) {
        err = fffs_flash_span_is_erased(fs, (size_t)sector * fs->sector_size,
                fs->sector_size);
        if (err != FFFS_OK) {
            return err;
        }
        if (footer_off < FFFS_MD_FILE_RECORD_SIZE ||
                footer_off - FFFS_MD_FILE_RECORD_SIZE < min_free) {
            return FFFS_ERR_NO_SPACE;
        }
        *data_off = 0;
        *record_off = (uint16_t)(footer_off - FFFS_MD_FILE_RECORD_SIZE);
        *needs_footer = true;
        return FFFS_OK;
    }

    enum fffs_lifecycle_object_state footer_state =
        fffs_lifecycle_decode_footer(footer[5]);
    if (footer[4] != FFFS_SECTOR_TYPE_FILE ||
            footer[6] != 0xff || footer[7] != 0xff ||
            memcmp(footer + 8, FFFS_SECTOR_MAGIC, 4) != 0 ||
            footer_state != FFFS_LIFECYCLE_OBJECT_LIVE) {
        return FFFS_ERR_NO_SPACE;
    }
    if (fffs_lifecycle_hint_pair(footer[5]) ==
            FFFS_BITMIRROR_CLEARED) {
        return FFFS_ERR_NO_SPACE;
    }

    size_t cursor = footer_off;
    size_t max_data_end = 0;
    size_t metadata_start = footer_off;
    size_t record_count = 0;
    struct file_md_reader reader = {
        .sector = sector,
    };
    while (cursor > FFFS_SECTOR_FOOTER_SIZE) {
        struct decoded_file_md md;
        enum decoded_file_md_state md_state;
        err = decode_file_md_record(fs, &reader, cursor, &md, &md_state);
        if (err != FFFS_OK) {
            return err;
        }
        if (md_state == FFFS_DECODED_MD_ERASED) {
            break;
        }
        if (md_state == FFFS_DECODED_MD_INVALID) {
            return FFFS_ERR_NO_SPACE;
        }
        record_count++;
        if (md_state == FFFS_DECODED_MD_LIVE && md.slot == reject_slot) {
            return FFFS_ERR_NO_SPACE;
        }
        size_t data_end = (size_t)md.data_off + md.data_len;
        if (data_end > max_data_end) {
            max_data_end = data_end;
        }
        cursor = md.record_start;
        metadata_start = md.record_start;
        if (cursor <= max_data_end) {
            break;
        }
    }

    if (record_count >= FFFS_ALLOC_MAX_MD_RECORDS_PER_SECTOR) {
        (void)fffs_mark_sector_full(fs, sector);
        return FFFS_ERR_NO_SPACE;
    }

    size_t new_record_off = metadata_start - FFFS_MD_FILE_RECORD_SIZE;
    if (new_record_off < max_data_end ||
            new_record_off - max_data_end < min_free) {
        (void)fffs_mark_sector_full(fs, sector);
        return FFFS_ERR_NO_SPACE;
    }
    err = fffs_flash_span_is_erased(fs, (size_t)sector * fs->sector_size +
            max_data_end, new_record_off - max_data_end);
    if (err != FFFS_OK) {
        return err;
    }

    *data_off = (uint16_t)max_data_end;
    *record_off = (uint16_t)new_record_off;
    *needs_footer = false;
    return FFFS_OK;
}

int fffs_write_extent_metadata(struct fffs_file *file, uint16_t sector,
        uint32_t serial, uint16_t data_off, uint16_t record_off,
        bool write_footer, uint16_t data_len, uint32_t total_size,
        uint16_t next, uint32_t file_offset, bool commit_index) {
    struct fffs *fs = file->fs;
    uint8_t tail[FFFS_MD_FILE_RECORD_SIZE + FFFS_SECTOR_FOOTER_SIZE];
    uint8_t *md = tail;
    uint8_t *footer = tail + FFFS_MD_FILE_RECORD_SIZE;
    memset(tail, 0xff, sizeof(tail));
    md[0] = FFFS_MD_FLAGS_VALID;
    store16(md + 1, file->slot);
    store16(md + 3, next);
    store16(md + 5, 1);
    store16(md + 7, data_off);
    store16(md + 9, commit_index ?
            (uint16_t)(file->root_payload_offset + data_len) : data_len);
    store32(md + 11, commit_index ? total_size : file_offset);
    md[15] = commit_index ? FFFS_MD_TYPE_FILE_ROOT_V1 :
        FFFS_MD_TYPE_FILE_CONT_V1;
    encode_sector_footer(footer, serial);

    size_t off = (size_t)sector * fs->sector_size + record_off;
    int err = write_footer ?
        fffs_flash_program_aligned(fs, off, tail, sizeof(tail)) :
        fffs_flash_program_aligned(fs, off, md, FFFS_MD_FILE_RECORD_SIZE);
    if (err != FFFS_OK) {
        return err;
    }
    return commit_index ? fffs_append_index_record(fs, file->slot, file->head)
        : FFFS_OK;
}
