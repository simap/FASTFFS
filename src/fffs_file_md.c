/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS file metadata records, metadata walkers, slot lookup, tombstones,
 * and free-window scans.
 */

#include "fffs_internal.h"

#include <string.h>

static int read_root_prefix(struct fffs *fs, uint16_t sector,
        uint16_t data_off, uint16_t data_len, uint32_t size,
        struct fffs_stat *st, uint16_t *payload_data_off,
        uint16_t *payload_data_len, struct fffs_read_cache_view *cache) {
    if (data_len < 2) {
        return FFFS_ERR_CORRUPT;
    }
    uint8_t local[FFFS_MAX_NAME + 2u];
    bool use_cache = cache && cache->data && cache->capacity >= sizeof(local);
    uint8_t *buf = use_cache ? cache->data : local;
    size_t buf_size = use_cache ? cache->capacity : sizeof(local);
    size_t nread = data_len < buf_size ? data_len : buf_size;
    int err = fffs_flash_read(fs, (size_t)sector * fs->sector_size +
            data_off, buf, nread);
    if (err != FFFS_OK) {
        return err;
    }
    size_t name_len = buf[0];
    size_t vmeta_len_off = 1u + name_len;
    if (name_len == 0 || name_len > FFFS_MAX_NAME ||
            vmeta_len_off >= data_len || vmeta_len_off >= nread) {
        return FFFS_ERR_CORRUPT;
    }

    size_t vmeta_len = buf[vmeta_len_off];
    size_t payload_off = 1u + name_len + 1u + vmeta_len;
    if (payload_off > data_len) {
        return FFFS_ERR_CORRUPT;
    }

    if (st) {
        memcpy(st->name, buf + 1, name_len);
        st->name[name_len] = '\0';
        st->size = size;
    }
    if (payload_data_off) {
        *payload_data_off = (uint16_t)(data_off + payload_off);
    }
    if (payload_data_len) {
        *payload_data_len = (uint16_t)(data_len - payload_off);
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

static int decode_file_md_record(struct fffs *fs,
        struct fffs_sector_reader *reader, uint16_t sector, size_t cursor,
        struct fffs_md_record *record, enum fffs_md_walk_result *result) {
    *result = FFFS_MD_WALK_END_INVALID;
    memset(record, 0, sizeof(*record));
    const uint8_t *type;
    int err = fffs_sector_reader_view(fs, reader, sector, cursor - 1u,
            sizeof(*type), &type);
    if (err != FFFS_OK) {
        return err;
    }
    if (*type == 0xff) {
        *result = FFFS_MD_WALK_END_ERASED;
        return FFFS_OK;
    }

    size_t record_len = FFFS_MD_FILE_RECORD_SIZE;
    if (cursor < FFFS_SECTOR_FOOTER_SIZE + record_len) {
        return FFFS_ERR_CORRUPT;
    }
    size_t record_start = cursor - record_len;
    record->type = *type;
    record->record_start = record_start;
    record->record_len = record_len;

    if (*type == FFFS_MD_TYPE_FILE_ROOT_V1 ||
            *type == FFFS_MD_TYPE_FILE_CONT_V1) {
        /* Current file metadata records have one fixed size. */
    } else {
        return FFFS_OK;
    }

    const uint8_t *buf;
    err = fffs_sector_reader_view(fs, reader, sector, record_start,
            record_len, &buf);
    if (err != FFFS_OK) {
        return err;
    }
    if (fffs_flash_bytes_erased(buf, record_len)) {
        *result = FFFS_MD_WALK_END_ERASED;
        return FFFS_OK;
    }

    enum fffs_bitmirror_state valid = fffs_lifecycle_valid_pair(buf[0]);
    enum fffs_bitmirror_state tombstone =
        fffs_lifecycle_tombstone_pair(buf[0]);
    bool live = fffs_lifecycle_is_live(valid, tombstone);
    record->state = buf[0];
    record->live = live;
    record->slot = fffs_load_le16(buf + 1);
    record->next = fffs_load_le16(buf + 3);
    record->span_len = fffs_load_le16(buf + 5);
    record->data_off = fffs_load_le16(buf + 7);
    record->data_len = fffs_load_le16(buf + 9);
    record->size_or_offset = fffs_load_le32(buf + 11);
    if (buf[0] != 0xff && !live && valid != FFFS_BITMIRROR_CLEARED) {
        return FFFS_OK;
    }

    bool uncommitted = buf[0] == 0xff;
    if ((size_t)record->data_off + record->data_len > record_start) {
        return FFFS_OK;
    }
    if (!uncommitted) {
        if (record->span_len == 0 ||
                (size_t)sector + record->span_len >
                    fs->sector_count) {
            return FFFS_OK;
        }
        if (record->next != 0 &&
                (record->next < fs->index_sectors ||
                 record->next >= fs->sector_count)) {
            return FFFS_OK;
        }
    }
    *result = FFFS_MD_WALK_RECORD;
    return FFFS_OK;
}

static bool valid_data_sector(const struct fffs *fs, uint16_t sector) {
    return sector >= fs->index_sectors && sector < fs->sector_count;
}

enum fffs_md_slot_match {
    FFFS_MD_MATCH_LIVE,
    FFFS_MD_MATCH_UNCOMMITTED,
};

int fffs_md_walk_init(struct fffs *fs, struct fffs_md_walk *walk,
        uint16_t sector, struct fffs_sector_reader *window) {
    if (!fs || !walk || !window || !window->data ||
            window->capacity < FFFS_MD_FILE_RECORD_SIZE +
                FFFS_SECTOR_FOOTER_SIZE ||
            sector < fs->index_sectors || sector >= fs->sector_count) {
        return FFFS_ERR_INVALID;
    }
    *walk = (struct fffs_md_walk){
        .sector = sector,
        .cursor = fs->sector_size - FFFS_SECTOR_FOOTER_SIZE,
        .claimed_data_end = 0,
        .live_seen = false,
        .active = true,
    };
    if (window->sector != sector) {
        window->sector = sector;
        window->start = 0;
        window->len = 0;
    }
    return FFFS_OK;
}

static bool md_record_matches_slot(const struct fffs_md_record *record,
        uint16_t want_slot, enum fffs_md_slot_match match) {
    if (record->slot != want_slot) {
        return false;
    }
    if (match == FFFS_MD_MATCH_UNCOMMITTED) {
        return record->state == 0xff;
    }
    return record->live;
}

int fffs_md_walk_next(struct fffs *fs, struct fffs_md_walk *walk,
        struct fffs_sector_reader *window, struct fffs_md_record *record,
        enum fffs_md_walk_result *result) {
    if (!fs || !walk || !window || !record || !result ||
            !walk->active || walk->sector != window->sector) {
        return FFFS_ERR_INVALID;
    }

    if (walk->cursor <= FFFS_SECTOR_FOOTER_SIZE) {
        *result = FFFS_MD_WALK_END_CLAIMED_DATA;
        walk->active = false;
        return FFFS_OK;
    }

    int err = decode_file_md_record(fs, window, walk->sector, walk->cursor,
            record, result);
    if (err != FFFS_OK) {
        return err;
    }
    if (*result != FFFS_MD_WALK_RECORD) {
        walk->active = false;
        return FFFS_OK;
    }

    walk->cursor = record->record_start;
    size_t data_end = (size_t)record->data_off + record->data_len;
    if (data_end > walk->claimed_data_end) {
        walk->claimed_data_end = data_end;
    }

    if (walk->cursor <= walk->claimed_data_end) {
        walk->active = false;
    }
    return FFFS_OK;
}

static int read_md_for_slot(struct fffs *fs, uint16_t sector,
        uint16_t want_slot, enum fffs_md_slot_match match,
        struct fffs_md_record *out) {
    if (!out || !valid_data_sector(fs, sector)) {
        return FFFS_ERR_CORRUPT;
    }

    uint8_t window_buf[FFFS_MD_PRELOAD_MAX];
    struct fffs_sector_reader window = {
        .data = window_buf,
        .capacity = sizeof(window_buf),
        .reverse = true,
    };
    struct fffs_sector_footer footer;
    const uint8_t *footer_bytes;
    int err = fffs_sector_reader_view(fs, &window, sector,
            fs->sector_size - FFFS_SECTOR_FOOTER_SIZE,
            FFFS_SECTOR_FOOTER_SIZE, &footer_bytes);
    if (err != FFFS_OK) {
        return err;
    }
    fffs_decode_sector_footer(footer_bytes, &footer);
    if (footer.erased || footer.type != FFFS_SECTOR_TYPE_FILE ||
            !footer.magic_valid ||
            !fffs_lifecycle_is_live(footer.valid_bits,
                footer.tombstone_bits)) {
        return match == FFFS_MD_MATCH_UNCOMMITTED ?
            FFFS_ERR_NOT_FOUND : FFFS_ERR_CORRUPT;
    }

    struct fffs_md_walk walk;
    err = fffs_md_walk_init(fs, &walk, sector, &window);
    if (err != FFFS_OK) {
        return err;
    }
    struct fffs_md_record record;
    while (walk.active) {
        enum fffs_md_walk_result result;
        err = fffs_md_walk_next(fs, &walk, &window, &record, &result);
        if (err != FFFS_OK) {
            return err;
        }
        if (result != FFFS_MD_WALK_RECORD) {
            return FFFS_ERR_CORRUPT;
        }
        if (!md_record_matches_slot(&record, want_slot, match)) {
            continue;
        }

        *out = record;
        return FFFS_OK;
    }
    return match == FFFS_MD_MATCH_UNCOMMITTED ?
        FFFS_ERR_NOT_FOUND : FFFS_ERR_CORRUPT;
}

int fffs_read_md_for_slot(struct fffs *fs, uint16_t sector,
        uint16_t want_slot, struct fffs_md_record *out) {
    return read_md_for_slot(fs, sector, want_slot, FFFS_MD_MATCH_LIVE, out);
}

int fffs_read_file_root_md(struct fffs *fs, uint16_t sector,
        uint16_t want_slot, struct fffs_stat *st,
        uint16_t *data_off, uint16_t *data_len,
        uint16_t *next, uint16_t *span_len, uint32_t *root_size,
        struct fffs_read_cache_view *cache) {
    struct fffs_md_record record;
    int err = fffs_read_md_for_slot(fs, sector, want_slot, &record);
    if (err != FFFS_OK) {
        return err;
    }
    if (record.type != FFFS_MD_TYPE_FILE_ROOT_V1) {
        return FFFS_ERR_CORRUPT;
    }

    uint16_t payload_data_off = record.data_off;
    uint16_t payload_data_len = record.data_len;
    if (st || data_off || data_len || cache) {
        err = read_root_prefix(fs, sector, record.data_off,
                record.data_len, record.size_or_offset, st,
                &payload_data_off, &payload_data_len, cache);
        if (err != FFFS_OK) {
            return err;
        }
    }

    if (data_off) {
        *data_off = payload_data_off;
    }
    if (data_len) {
        *data_len = payload_data_len;
    }
    if (next) {
        *next = record.next;
    }
    if (span_len) {
        *span_len = record.span_len;
    }
    if (root_size) {
        *root_size = record.size_or_offset;
    }
    return FFFS_OK;
}

int fffs_tombstone_metadata_for_slot(struct fffs *fs, uint16_t sector,
        uint16_t want_slot, enum fffs_tombstone_accounting accounting,
        bool *accounted) {
    if (!valid_data_sector(fs, sector)) {
        return FFFS_ERR_CORRUPT;
    }
    if (accounted) {
        *accounted = false;
    }

    uint8_t window_buf[FFFS_MD_PRELOAD_MAX];
    struct fffs_sector_reader window = {
        .data = window_buf,
        .capacity = sizeof(window_buf),
        .reverse = true,
    };
    struct fffs_sector_footer footer;
    const uint8_t *footer_bytes;
    int err = fffs_sector_reader_view(fs, &window, sector,
            fs->sector_size - FFFS_SECTOR_FOOTER_SIZE,
            FFFS_SECTOR_FOOTER_SIZE, &footer_bytes);
    if (err != FFFS_OK) {
        return err;
    }
    fffs_decode_sector_footer(footer_bytes, &footer);
    if (footer.erased) {
        return FFFS_OK;
    }
    if (footer.type != FFFS_SECTOR_TYPE_FILE || !footer.magic_valid) {
        return FFFS_ERR_CORRUPT;
    }
    bool live = fffs_lifecycle_is_live(footer.valid_bits,
            footer.tombstone_bits);
    bool tombstoned = footer.valid_bits != FFFS_BITMIRROR_MIXED &&
        footer.tombstone_bits == FFFS_BITMIRROR_CLEARED;
    if (tombstoned) {
        return FFFS_OK;
    }
    if (!live) {
        return FFFS_ERR_CORRUPT;
    }

    struct fffs_md_walk walk;
    err = fffs_md_walk_init(fs, &walk, sector, &window);
    if (err != FFFS_OK) {
        return err;
    }
    struct fffs_md_record record;
    while (walk.active) {
        enum fffs_md_walk_result result;
        err = fffs_md_walk_next(fs, &walk, &window, &record, &result);
        if (err != FFFS_OK) {
            return err;
        }
        if (result == FFFS_MD_WALK_END_INVALID) {
            return FFFS_ERR_CORRUPT;
        }
        if (result != FFFS_MD_WALK_RECORD) {
            return FFFS_OK;
        }
        if (!record.live || record.slot != want_slot) {
            continue;
        }

        uint8_t tombstone = FFFS_MD_FLAGS_TOMBSTONED;
        int err = fffs_flash_program_aligned(fs,
                (size_t)sector * fs->sector_size + record.record_start,
                &tombstone, sizeof(tombstone));
        if (err == FFFS_OK &&
                accounting == FFFS_TOMBSTONE_COMMITTED_DELETE &&
                record.type == FFFS_MD_TYPE_FILE_ROOT_V1) {
            fffs_fsinfo_note_committed_delete(fs, record.size_or_offset);
            if (accounted) {
                *accounted = true;
            }
        }
        return err;
    }
    return FFFS_OK;
}

static int read_uncommitted_metadata_for_slot(struct fffs *fs, uint16_t sector,
        uint16_t want_slot, struct fffs_md_record *out) {
    return read_md_for_slot(fs, sector, want_slot,
            FFFS_MD_MATCH_UNCOMMITTED, out);
}

int fffs_find_sector_free_window(struct fffs *fs, uint16_t sector,
        uint16_t min_free, uint16_t reject_slot, uint16_t *data_off,
        uint16_t *record_off, bool *needs_footer, uint16_t *md_records) {
    if (sector < fs->index_sectors || sector >= fs->sector_count ||
            !data_off || !record_off || !needs_footer || !md_records) {
        return FFFS_ERR_INVALID;
    }

    uint8_t window_buf[FFFS_MD_PRELOAD_MAX];
    struct fffs_sector_reader window = {
        .data = window_buf,
        .capacity = sizeof(window_buf),
        .reverse = true,
    };
    struct fffs_sector_footer view;
    const uint8_t *footer;
    int err = fffs_sector_reader_view(fs, &window, sector,
            fs->sector_size - FFFS_SECTOR_FOOTER_SIZE,
            FFFS_SECTOR_FOOTER_SIZE, &footer);
    if (err != FFFS_OK) {
        return err;
    }
    fffs_decode_sector_footer(footer, &view);

    size_t footer_off = fs->sector_size - FFFS_SECTOR_FOOTER_SIZE;
    if (view.erased) {
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
        *md_records = 0;
        return FFFS_OK;
    }

    if (view.type != FFFS_SECTOR_TYPE_FILE || !view.magic_valid ||
            !fffs_lifecycle_is_live(view.valid_bits, view.tombstone_bits)) {
        return FFFS_ERR_NO_SPACE;
    }
    if (view.full_bits == FFFS_BITMIRROR_CLEARED) {
        return FFFS_ERR_NO_SPACE;
    }

    size_t max_data_end = 0;
    size_t metadata_start = footer_off;
    size_t record_count = 0;
    struct fffs_md_walk walk;
    err = fffs_md_walk_init(fs, &walk, sector, &window);
    if (err != FFFS_OK) {
        return err;
    }
    while (walk.active) {
        struct fffs_md_record record;
        enum fffs_md_walk_result result;
        err = fffs_md_walk_next(fs, &walk, &window, &record, &result);
        if (err != FFFS_OK) {
            return err;
        }
        if (result == FFFS_MD_WALK_END_ERASED) {
            break;
        }
        if (result == FFFS_MD_WALK_END_INVALID) {
            return FFFS_ERR_NO_SPACE;
        }
        if (result != FFFS_MD_WALK_RECORD) {
            break;
        }
        record_count++;
        if (record.live && record.slot == reject_slot) {
            return FFFS_ERR_NO_SPACE;
        }
        size_t data_end = (size_t)record.data_off + record.data_len;
        if (data_end > max_data_end) {
            max_data_end = data_end;
        }
        metadata_start = record.record_start;
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
    *md_records = (uint16_t)record_count;
    return FFFS_OK;
}

static uint32_t claim_sector_serial(struct fffs *fs) {
    uint32_t serial = fs->next_sector_serial++;
    if (fs->next_sector_serial == 0) {
        fs->next_sector_serial = 1;
    }
    return serial;
}

static int program_extent_metadata(struct fffs_file *file, uint16_t sector,
        uint16_t data_off, uint16_t record_off, bool write_footer,
        uint16_t data_len, uint16_t span_len, uint32_t total_size,
        uint16_t next, uint32_t file_offset, bool is_root, bool live) {
    struct fffs *fs = file->fs;
    uint8_t tail[FFFS_MD_FILE_RECORD_SIZE + FFFS_SECTOR_FOOTER_SIZE];
    uint8_t *md = tail;
    uint8_t *footer = tail + FFFS_MD_FILE_RECORD_SIZE;
    memset(tail, 0xff, sizeof(tail));
    if (live) {
        md[0] = FFFS_MD_FLAGS_VALID;
        fffs_store_le16(md + 3, next);
        fffs_store_le16(md + 5, span_len);
    }
    fffs_store_le16(md + 1, file->slot);
    fffs_store_le16(md + 7, data_off);
    fffs_store_le16(md + 9, is_root ?
            (uint16_t)(file->root_payload_offset + data_len) : data_len);
    if (is_root) {
        if (live) {
            fffs_store_le32(md + 11, total_size);
        }
    } else {
        fffs_store_le32(md + 11, file_offset);
    }
    md[15] = is_root ? FFFS_MD_TYPE_FILE_ROOT_V1 :
        FFFS_MD_TYPE_FILE_CONT_V1;
    if (write_footer) {
        fffs_encode_sector_footer(footer, claim_sector_serial(fs), true);
    }

    size_t off = (size_t)sector * fs->sector_size + record_off;
    int err = write_footer ?
        fffs_flash_program_aligned(fs, off, tail, sizeof(tail)) :
        fffs_flash_program_aligned(fs, off, md, FFFS_MD_FILE_RECORD_SIZE);
    if (err != FFFS_OK) {
        return err;
    }
    size_t logical_data_len = is_root ?
        (size_t)file->root_payload_offset + data_len : data_len;
    if ((size_t)data_off + logical_data_len >= record_off) {
        err = fffs_mark_sector_full(fs, sector);
        if (err != FFFS_OK) {
            return err;
        }
        fffs_alloc_map_mark_used(fs, sector);
    }
    return FFFS_OK;
}

int fffs_begin_extent_metadata(struct fffs_file *file, uint16_t sector,
        uint16_t data_off, uint16_t record_off, bool write_footer,
        uint16_t data_len, uint32_t file_offset) {
    bool is_root = sector == file->head;
    return program_extent_metadata(file, sector, data_off, record_off,
            write_footer, data_len, 0, 0, 0, file_offset, is_root, false);
}

int fffs_finish_extent_metadata(struct fffs_file *file, uint16_t sector,
        uint16_t next, uint16_t span_len, uint32_t total_size,
        bool write_size, bool make_live, bool commit_index) {
    struct fffs_md_record record;
    int err = read_uncommitted_metadata_for_slot(file->fs, sector,
            file->slot, &record);
    if (err != FFFS_OK) {
        return err;
    }

    size_t off = (size_t)sector * file->fs->sector_size + record.record_start;
    uint8_t link[4];
    fffs_store_le16(link, next);
    fffs_store_le16(link + 2, span_len);
    err = fffs_flash_program_aligned(file->fs, off + 3u, link, sizeof(link));
    if (err != FFFS_OK) {
        return err;
    }
    if (write_size) {
        uint8_t size_buf[4];
        fffs_store_le32(size_buf, total_size);
        err = fffs_flash_program_aligned(file->fs, off + 11u,
                size_buf, sizeof(size_buf));
        if (err != FFFS_OK) {
            return err;
        }
    }
    if (make_live) {
        uint8_t state = FFFS_MD_FLAGS_VALID;
        err = fffs_flash_program_aligned(file->fs, off, &state,
                sizeof(state));
        if (err != FFFS_OK) {
            return err;
        }
    }
    return commit_index ? fffs_append_index_record(file->fs, file->slot,
            file->head) : FFFS_OK;
}

int fffs_finish_deferred_root_metadata(struct fffs_file *file) {
    struct fffs_md_record root;
    int err = read_uncommitted_metadata_for_slot(file->fs, file->head,
            file->slot, &root);
    if (err != FFFS_OK) {
        return err;
    }
    return fffs_finish_extent_metadata(file, file->head, root.next,
            root.span_len, file->size, true, true, true);
}

int fffs_write_extent_metadata(struct fffs_file *file, uint16_t sector,
        uint16_t data_off, uint16_t record_off, bool write_footer,
        uint16_t data_len, uint16_t span_len, uint32_t total_size,
        uint16_t next, uint32_t file_offset, bool commit_index) {
    bool is_root = sector == file->head;
    int err = program_extent_metadata(file, sector, data_off, record_off,
            write_footer, data_len, span_len, total_size, next, file_offset,
            is_root, true);
    return err == FFFS_OK && commit_index ?
        fffs_append_index_record(file->fs, file->slot, file->head) : err;
}
