/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Ben Hencke
 *
 * FASTFFS host inspection support: read-only image decoding, fsck-style
 * consistency summaries, human-readable dumps, and deterministic workloads.
 */

#include "fastffs/fastffs_inspect.h"

#include "fffs_internal.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct decoded_md {
    uint8_t type;
    uint8_t state;
    uint16_t slot;
    uint16_t data_off;
    uint16_t data_len;
    uint16_t next;
    uint32_t size;
    char name[FFFS_MAX_NAME + 1];
};

enum md_state {
    MD_ERASED = 0,
    MD_VALID = 1,
    MD_TOMBSTONED = 2,
    MD_CORRUPT = 3,
};

static uint16_t load16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t load32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int backend_read(const struct fffs_backend *backend, size_t offset,
        void *buffer, size_t size) {
    if (offset > backend->size || size > backend->size - offset) {
        return FFFS_ERR_RANGE;
    }
    return fffs_map_backend_status(backend->read(backend->ctx, offset,
                buffer, size));
}

static bool span_erased(const uint8_t *p, size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (p[i] != 0xff) {
            return false;
        }
    }
    return true;
}

static bool valid_footer(const uint8_t footer[FFFS_SECTOR_FOOTER_SIZE],
        uint32_t *serial) {
    enum fffs_lifecycle_object_state footer_state =
        fffs_lifecycle_decode_footer(footer[5]);
    if (footer[4] != FFFS_SECTOR_TYPE_FILE ||
            footer[6] != 0xff || footer[7] != 0xff ||
            memcmp(footer + 8, FFFS_SECTOR_MAGIC, 4) != 0 ||
            (footer_state != FFFS_LIFECYCLE_OBJECT_LIVE &&
             footer_state != FFFS_LIFECYCLE_OBJECT_TOMBSTONED)) {
        return false;
    }
    if (serial) {
        *serial = load32(footer);
    }
    return true;
}

static enum md_state decode_md(const uint8_t md[FFFS_MD_SIZE],
        size_t data_limit, struct decoded_md *out) {
    if (span_erased(md, FFFS_MD_SIZE)) {
        return MD_ERASED;
    }
    enum fffs_lifecycle_object_state md_state =
        fffs_lifecycle_decode_md(md[0]);
    if (md_state != FFFS_LIFECYCLE_OBJECT_LIVE &&
            md_state != FFFS_LIFECYCLE_OBJECT_TOMBSTONED) {
        return MD_CORRUPT;
    }
    if (md[15] != FFFS_MD_TYPE_FILE_ROOT_V1 &&
            md[15] != FFFS_MD_TYPE_FILE_CONT_V1) {
        return MD_CORRUPT;
    }
    uint16_t data_off = load16(md + 7);
    uint16_t data_len = load16(md + 9);
    if ((size_t)data_off + data_len > data_limit || load16(md + 5) == 0) {
        return MD_CORRUPT;
    }

    if (out) {
        memset(out, 0, sizeof(*out));
        out->type = md[15];
        out->state = md[0];
        out->slot = load16(md + 1);
        out->data_off = data_off;
        out->data_len = data_len;
        out->next = load16(md + 3);
        if (md[15] == FFFS_MD_TYPE_FILE_ROOT_V1) {
            out->size = load32(md + 11);
        }
    }
    return md_state == FFFS_LIFECYCLE_OBJECT_TOMBSTONED ?
        MD_TOMBSTONED : MD_VALID;
}

static int read_root_name_for_decoded(const struct fffs_backend *backend,
        const struct fffs_inspect_summary *summary, size_t sector,
        struct decoded_md *decoded, enum md_state *state) {
    if (*state != MD_VALID ||
            decoded->type != FFFS_MD_TYPE_FILE_ROOT_V1) {
        return FFFS_OK;
    }

    uint8_t name_len = 0;
    int err = backend_read(backend, sector * summary->sector_size +
            decoded->data_off, &name_len, sizeof(name_len));
    if (err != FFFS_OK) {
        return err;
    }
    if (name_len == 0 || name_len > FFFS_MAX_NAME ||
            (size_t)1u + name_len + 1u > decoded->data_len) {
        *state = MD_CORRUPT;
        return FFFS_OK;
    }
    err = backend_read(backend, sector * summary->sector_size +
            decoded->data_off + 1u, decoded->name, name_len);
    if (err != FFFS_OK) {
        return err;
    }
    decoded->name[name_len] = '\0';
    return FFFS_OK;
}

static int read_record_before(const struct fffs_backend *backend,
        const struct fffs_inspect_summary *summary, size_t sector,
        size_t cursor, struct decoded_md *decoded, enum md_state *state,
        size_t *record_start) {
    if (cursor < FFFS_MD_FILE_RECORD_SIZE) {
        *state = MD_CORRUPT;
        return FFFS_OK;
    }

    uint8_t type;
    int err = backend_read(backend, sector * summary->sector_size +
            cursor - 1u, &type, sizeof(type));
    if (err != FFFS_OK) {
        return err;
    }
    if (type == 0xff) {
        *state = MD_ERASED;
        return FFFS_OK;
    }

    size_t len = FFFS_MD_FILE_RECORD_SIZE;
    if (cursor < len) {
        *state = MD_CORRUPT;
        return FFFS_OK;
    }
    uint8_t md[FFFS_MD_SIZE];
    size_t start = cursor - len;
    err = backend_read(backend, sector * summary->sector_size + start,
            md, sizeof(md));
    if (err != FFFS_OK) {
        return err;
    }
    *state = decode_md(md, start, decoded);
    if (record_start) {
        *record_start = start;
    }
    return read_root_name_for_decoded(backend, summary, sector, decoded,
            state);
}

static int discover(const struct fffs_backend *backend,
        struct fffs_inspect_summary *summary) {
    if (!backend || !backend->ctx || !backend->size || !backend->read) {
        return FFFS_ERR_INVALID;
    }

    struct fffs_index_sequence sequence;
    int err = fffs_find_index_sequence(backend, &sequence);
    if (err != FFFS_OK) {
        return err;
    }

    size_t sector_size = (size_t)256u << sequence.sector_shift;
    if (backend->size % sector_size != 0 ||
            backend->size / sector_size > UINT16_MAX) {
        return FFFS_ERR_CORRUPT;
    }

    memset(summary, 0, sizeof(*summary));
    summary->sector_size = sector_size;
    summary->sector_count = backend->size / sector_size;
    summary->index_sectors = sequence.index_sectors;
    summary->active_index_sector = sequence.active_sector;
    summary->active_index_serial = sequence.active_serial;
    return FFFS_OK;
}

static int replay_active_index(const struct fffs_backend *backend,
        const struct fffs_inspect_summary *summary, uint16_t *live_heads) {
    size_t off = summary->active_index_sector * summary->sector_size +
        FFFS_HEADER_SIZE;
    size_t end = (summary->active_index_sector + 1) * summary->sector_size;

    for (; off + 4 <= end; off += 4) {
        uint8_t rec[4];
        int err = backend_read(backend, off, rec, sizeof(rec));
        if (err != FFFS_OK) {
            return err;
        }
        uint16_t slot = load16(rec);
        uint16_t head = load16(rec + 2);
        if (slot == UINT16_MAX && head == UINT16_MAX) {
            return FFFS_OK;
        }
        if (slot == 0 && head == 0) {
            return FFFS_ERR_CORRUPT;
        }
        if (head == 0) {
            live_heads[slot] = 0;
            continue;
        }
        if (head < summary->index_sectors ||
                head >= summary->sector_count) {
            return FFFS_ERR_CORRUPT;
        }
        live_heads[slot] = head;
    }
    return FFFS_OK;
}

static int inspect_index(const struct fffs_backend *backend,
        struct fffs_inspect_summary *summary, uint16_t *live_heads) {
    size_t off = summary->active_index_sector * summary->sector_size +
        FFFS_HEADER_SIZE;
    size_t end = (summary->active_index_sector + 1) * summary->sector_size;

    for (; off + 4 <= end; off += 4) {
        uint8_t rec[4];
        int err = backend_read(backend, off, rec, sizeof(rec));
        if (err != FFFS_OK) {
            return err;
        }
        uint16_t slot = load16(rec);
        uint16_t head = load16(rec + 2);
        if (slot == UINT16_MAX && head == UINT16_MAX) {
            break;
        }
        if (slot == 0 && head == 0) {
            summary->index_corrupt_records += 1;
            break;
        }
        summary->index_records += 1;
        if (head == 0) {
            summary->index_deletes += 1;
            live_heads[slot] = 0;
            continue;
        }
        if (head < summary->index_sectors ||
                head >= summary->sector_count) {
            summary->index_corrupt_records += 1;
            continue;
        }
        live_heads[slot] = head;
    }

    for (size_t slot = 0; slot < FFFS_SLOT_COUNT; slot++) {
        if (live_heads[slot] != 0) {
            summary->live_entries += 1;
        }
    }
    return FFFS_OK;
}

static int read_decoded_md(const struct fffs_backend *backend,
        const struct fffs_inspect_summary *summary, size_t sector,
        uint16_t want_slot, struct decoded_md *decoded,
        enum md_state *state) {
    uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
    size_t footer_off = sector * summary->sector_size +
        summary->sector_size - FFFS_SECTOR_FOOTER_SIZE;
    int err = backend_read(backend, footer_off, footer, sizeof(footer));
    if (err != FFFS_OK) {
        return err;
    }
    if (!valid_footer(footer, NULL)) {
        *state = MD_CORRUPT;
        return FFFS_OK;
    }

    size_t cursor = summary->sector_size - FFFS_SECTOR_FOOTER_SIZE;
    size_t claimed_data_end = 0;
    while (cursor > FFFS_SECTOR_FOOTER_SIZE) {
        size_t record_start = 0;
        err = read_record_before(backend, summary, sector, cursor, decoded,
                state, &record_start);
        if (err != FFFS_OK) {
            return err;
        }
        if (*state == MD_ERASED || *state == MD_CORRUPT) {
            return FFFS_OK;
        }
        cursor = record_start;
        size_t data_end = (size_t)decoded->data_off + decoded->data_len;
        if (data_end > claimed_data_end) {
            claimed_data_end = data_end;
        }
        if (*state == MD_TOMBSTONED || decoded->slot != want_slot) {
            if (cursor <= claimed_data_end) {
                break;
            }
            continue;
        }
        break;
    }
    return FFFS_OK;
}

static int record_reachable_for_slot(const struct fffs_backend *backend,
        const struct fffs_inspect_summary *summary,
        const uint16_t *live_heads, uint16_t slot, size_t sector,
        bool *reachable) {
    *reachable = false;
    uint16_t current = live_heads[slot];
    for (size_t depth = 0; current != 0 &&
            depth < summary->sector_count; depth++) {
        if (current < summary->index_sectors ||
                current >= summary->sector_count) {
            return FFFS_OK;
        }
        if (current == sector) {
            *reachable = true;
            return FFFS_OK;
        }

        struct decoded_md decoded;
        enum md_state state;
        int err = read_decoded_md(backend, summary, current, slot,
                &decoded, &state);
        if (err != FFFS_OK) {
            return err;
        }
        if (state != MD_VALID || decoded.slot != slot) {
            return FFFS_OK;
        }
        current = decoded.next;
    }
    return FFFS_OK;
}

static int mark_live_chains(const struct fffs_backend *backend,
        struct fffs_inspect_summary *summary, const uint16_t *live_heads,
        bool *reachable, bool *seen_root) {
    for (size_t slot = 0; slot < FFFS_SLOT_COUNT; slot++) {
        uint16_t head = live_heads[slot];
        if (head == 0) {
            continue;
        }

        uint16_t current = head;
        bool corrupt = false;
        for (size_t depth = 0; current != 0 &&
                depth < summary->sector_count; depth++) {
            if (current < summary->index_sectors ||
                    current >= summary->sector_count) {
                corrupt = true;
                break;
            }
            struct decoded_md decoded;
            enum md_state state;
            int err = read_decoded_md(backend, summary, current,
                    (uint16_t)slot, &decoded, &state);
            if (err != FFFS_OK) {
                return err;
            }
            if (state != MD_VALID || decoded.slot != slot) {
                corrupt = true;
                break;
            }

            reachable[current] = true;
            if (current == head) {
                seen_root[slot] = true;
            }
            current = decoded.next;
        }
        if (corrupt || !seen_root[slot]) {
            summary->live_entries_corrupt += 1;
        }
    }
    return FFFS_OK;
}

static int inspect_data_sectors(const struct fffs_backend *backend,
        struct fffs_inspect_summary *summary, const uint16_t *live_heads) {
    for (size_t sector = summary->index_sectors;
            sector < summary->sector_count; sector++) {
        uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
        size_t footer_off = sector * summary->sector_size +
            summary->sector_size - FFFS_SECTOR_FOOTER_SIZE;
        int err = backend_read(backend, footer_off, footer, sizeof(footer));
        if (err != FFFS_OK) {
            return err;
        }

        if (span_erased(footer, sizeof(footer))) {
            summary->data_sectors_erased += 1;
            continue;
        }
        if (!valid_footer(footer, NULL)) {
            summary->data_sectors_corrupt += 1;
            continue;
        }
        if (fffs_lifecycle_decode_footer(footer[5]) ==
                FFFS_LIFECYCLE_OBJECT_TOMBSTONED) {
            summary->data_sectors_tombstoned += 1;
            summary->md_tombstoned += 1;
            continue;
        }
        summary->data_sectors_owned += 1;

        size_t cursor = summary->sector_size - FFFS_SECTOR_FOOTER_SIZE;
        size_t claimed_data_end = 0;
        while (cursor > FFFS_SECTOR_FOOTER_SIZE) {
            struct decoded_md decoded;
            enum md_state state;
            size_t record_start = 0;
            err = read_record_before(backend, summary, sector, cursor,
                    &decoded, &state, &record_start);
            if (err != FFFS_OK) {
                return err;
            }
            if (state == MD_ERASED) {
                break;
            }
            if (state == MD_CORRUPT) {
                summary->md_corrupt += 1;
                break;
            }
            cursor = record_start;
            size_t data_end = (size_t)decoded.data_off + decoded.data_len;
            if (data_end > claimed_data_end) {
                claimed_data_end = data_end;
            }
            if (state == MD_TOMBSTONED) {
                if (cursor <= claimed_data_end) {
                    break;
                }
                summary->md_tombstoned += 1;
                continue;
            }

            bool record_live = false;
            err = record_reachable_for_slot(backend, summary, live_heads,
                    decoded.slot, sector, &record_live);
            if (err != FFFS_OK) {
                return err;
            }
            if (record_live) {
                summary->md_live += 1;
            } else {
                summary->md_obsolete_orphaned += 1;
            }
            if (cursor <= claimed_data_end) {
                break;
            }
        }
    }
    return FFFS_OK;
}

int fffs_inspect_check(const struct fffs_backend *backend,
        struct fffs_inspect_summary *summary) {
    if (!summary) {
        return FFFS_ERR_INVALID;
    }

    struct fffs_inspect_summary tmp;
    int err = discover(backend, &tmp);
    if (err != FFFS_OK) {
        return err;
    }

    uint16_t *live_heads = calloc(FFFS_SLOT_COUNT, sizeof(*live_heads));
    bool *seen_root = calloc(FFFS_SLOT_COUNT, sizeof(*seen_root));
    bool *reachable = calloc(tmp.sector_count, sizeof(*reachable));
    if (!live_heads || !seen_root || !reachable) {
        free(reachable);
        free(seen_root);
        free(live_heads);
        return FFFS_ERR_NOMEM;
    }

    err = inspect_index(backend, &tmp, live_heads);
    if (err == FFFS_OK) {
        err = mark_live_chains(backend, &tmp, live_heads, reachable,
                seen_root);
    }
    if (err == FFFS_OK) {
        err = inspect_data_sectors(backend, &tmp, live_heads);
    }

    free(reachable);
    free(seen_root);
    free(live_heads);
    if (err != FFFS_OK) {
        return err;
    }
    *summary = tmp;
    return FFFS_OK;
}

static const char *md_state_name(enum md_state state, bool live) {
    switch (state) {
    case MD_ERASED:
        return "orphaned-empty-md";
    case MD_VALID:
        return live ? "live" : "obsolete-orphaned";
    case MD_TOMBSTONED:
        return "tombstoned";
    case MD_CORRUPT:
        return "corrupt";
    default:
        return "unknown";
    }
}

static int dump_index_headers(const struct fffs_backend *backend, FILE *out,
        const struct fffs_inspect_summary *summary) {
    for (size_t sector = 0; sector < summary->index_sectors; sector++) {
        uint8_t hdr[FFFS_HEADER_SIZE];
        int err = backend_read(backend, sector * summary->sector_size,
                hdr, sizeof(hdr));
        if (err != FFFS_OK) {
            return err;
        }
        uint8_t count;
        uint8_t shift;
        uint8_t serial;
        if (fffs_valid_index_header(hdr, &count, &shift, &serial)) {
            fprintf(out, "index sector=%zu serial=%u active=%s\n",
                    sector, (unsigned)serial,
                    sector == summary->active_index_sector ? "yes" : "no");
        } else if (span_erased(hdr, sizeof(hdr))) {
            fprintf(out, "index sector=%zu erased\n", sector);
        } else {
            fprintf(out, "index sector=%zu invalid\n", sector);
        }
    }
    return FFFS_OK;
}

static int dump_active_index(const struct fffs_backend *backend, FILE *out,
        const struct fffs_inspect_summary *summary) {
    size_t off = summary->active_index_sector * summary->sector_size +
        FFFS_HEADER_SIZE;
    size_t end = (summary->active_index_sector + 1) * summary->sector_size;

    for (; off + 4 <= end; off += 4) {
        uint8_t rec[4];
        int err = backend_read(backend, off, rec, sizeof(rec));
        if (err != FFFS_OK) {
            return err;
        }
        uint16_t slot = load16(rec);
        uint16_t head = load16(rec + 2);
        if (slot == UINT16_MAX && head == UINT16_MAX) {
            break;
        }
        fprintf(out, "index-record slot=%u head=%u%s\n",
                (unsigned)slot, (unsigned)head,
                head == 0 ? " delete" : "");
    }
    return FFFS_OK;
}

static int dump_data_sectors(const struct fffs_backend *backend, FILE *out,
        const struct fffs_inspect_summary *summary,
        const uint16_t *live_heads) {
    for (size_t sector = summary->index_sectors;
            sector < summary->sector_count; sector++) {
        uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
        size_t footer_off = sector * summary->sector_size +
            summary->sector_size - FFFS_SECTOR_FOOTER_SIZE;
        int err = backend_read(backend, footer_off, footer, sizeof(footer));
        if (err != FFFS_OK) {
            return err;
        }
        if (span_erased(footer, sizeof(footer))) {
            continue;
        }

        uint32_t serial = 0;
        if (!valid_footer(footer, &serial)) {
            fprintf(out, "sector=%zu corrupt-footer\n", sector);
            continue;
        }

        size_t cursor = summary->sector_size - FFFS_SECTOR_FOOTER_SIZE;
        size_t claimed_data_end = 0;
        bool printed_any = false;
        while (cursor > FFFS_SECTOR_FOOTER_SIZE) {
            struct decoded_md decoded;
            enum md_state state;
            size_t record_start = 0;
            err = read_record_before(backend, summary, sector, cursor,
                    &decoded, &state, &record_start);
            if (err != FFFS_OK) {
                return err;
            }
            if (state == MD_ERASED) {
                break;
            }
            bool live = false;
            if (state == MD_VALID) {
                err = record_reachable_for_slot(backend, summary, live_heads,
                        decoded.slot, sector, &live);
                if (err != FFFS_OK) {
                    return err;
                }
            }
            fprintf(out, "sector=%zu serial=%" PRIu32 " md=%s",
                    sector, serial, md_state_name(state, live));
            if (state == MD_VALID || state == MD_TOMBSTONED) {
                fprintf(out, " slot=%u size=%" PRIu32 " name=%s",
                        (unsigned)decoded.slot, decoded.size, decoded.name);
            }
            fputc('\n', out);
            printed_any = true;
            if (state == MD_CORRUPT) {
                break;
            }
            cursor = record_start;
            size_t data_end = (size_t)decoded.data_off + decoded.data_len;
            if (data_end > claimed_data_end) {
                claimed_data_end = data_end;
            }
            if (cursor <= claimed_data_end) {
                break;
            }
        }
        if (!printed_any) {
            fprintf(out, "sector=%zu serial=%" PRIu32 " md=%s\n",
                    sector, serial, md_state_name(MD_ERASED, false));
        }
    }
    return FFFS_OK;
}

int fffs_inspect_dump(const struct fffs_backend *backend, FILE *out) {
    if (!out) {
        return FFFS_ERR_INVALID;
    }
    struct fffs_inspect_summary summary;
    int err = fffs_inspect_check(backend, &summary);
    if (err != FFFS_OK) {
        return err;
    }

    uint16_t *live_heads = calloc(FFFS_SLOT_COUNT, sizeof(*live_heads));
    bool *seen_root = calloc(FFFS_SLOT_COUNT, sizeof(*seen_root));
    bool *reachable = calloc(summary.sector_count, sizeof(*reachable));
    if (!live_heads || !seen_root || !reachable) {
        free(reachable);
        free(seen_root);
        free(live_heads);
        return FFFS_ERR_NOMEM;
    }
    err = replay_active_index(backend, &summary, live_heads);
    if (err != FFFS_OK) {
        free(reachable);
        free(seen_root);
        free(live_heads);
        return err;
    }
    err = mark_live_chains(backend, &summary, live_heads, reachable,
            seen_root);
    if (err != FFFS_OK) {
        free(reachable);
        free(seen_root);
        free(live_heads);
        return err;
    }

    fprintf(out, "FASTFFS image: sector_size=%zu sector_count=%zu "
            "index_sectors=%u active_index=%zu serial=%u\n",
            summary.sector_size, summary.sector_count,
            (unsigned)summary.index_sectors, summary.active_index_sector,
            (unsigned)summary.active_index_serial);
    fprintf(out, "summary: index_records=%zu live_entries=%zu "
            "live_corrupt=%zu data_tombstoned=%zu md_live=%zu "
            "md_obsolete_orphaned=%zu md_tombstoned=%zu md_corrupt=%zu\n",
            summary.index_records, summary.live_entries,
            summary.live_entries_corrupt, summary.data_sectors_tombstoned,
            summary.md_live, summary.md_obsolete_orphaned,
            summary.md_tombstoned, summary.md_corrupt);

    err = dump_index_headers(backend, out, &summary);
    if (err == FFFS_OK) {
        err = dump_active_index(backend, out, &summary);
    }
    if (err == FFFS_OK) {
        err = dump_data_sectors(backend, out, &summary, live_heads);
    }

    free(reachable);
    free(seen_root);
    free(live_heads);
    return err;
}

static uint32_t next_rand(uint32_t *state) {
    uint32_t x = *state ? *state : 1u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void fill_pattern(uint8_t *buffer, size_t size, uint8_t pattern) {
    for (size_t i = 0; i < size; i++) {
        buffer[i] = (uint8_t)(pattern + i);
    }
}

static size_t sectors_for_file(struct fffs *fs, size_t size) {
    size_t extent = fffs_max_file_data_size(fs);
    return (size + extent - 1) / extent;
}

static int workload_write_file(struct fffs *fs, const char *name,
        uint8_t *buffer, size_t size, uint8_t pattern) {
    fill_pattern(buffer, size, pattern);
    struct fffs_file file;
    int err = fffs_open(fs, &file, name,
            FFFS_O_WRONLY | FFFS_O_CREATE | FFFS_O_TRUNC);
    if (err != FFFS_OK) {
        return err;
    }
    size_t written = 0;
    err = fffs_write(&file, buffer, size, &written);
    if (err == FFFS_OK && written != size) {
        err = FFFS_ERR_IO;
    }
    if (err == FFFS_OK) {
        err = fffs_close(&file);
    }
    return err;
}

static int workload_read_verify(struct fffs *fs, const char *name,
        uint8_t *expected, uint8_t *readback, size_t size, uint8_t pattern) {
    fill_pattern(expected, size, pattern);
    struct fffs_file file;
    int err = fffs_open(fs, &file, name, FFFS_O_RDONLY);
    if (err != FFFS_OK) {
        return err;
    }
    size_t total = 0;
    while (total < size) {
        size_t nread = 0;
        err = fffs_read(&file, readback + total, size - total, &nread);
        if (err != FFFS_OK || nread == 0) {
            return err == FFFS_OK ? FFFS_ERR_CORRUPT : err;
        }
        total += nread;
    }
    err = fffs_close(&file);
    if (err != FFFS_OK) {
        return err;
    }
    return memcmp(expected, readback, size) == 0 ? FFFS_OK :
        FFFS_ERR_CORRUPT;
}

int fffs_workload_run(struct fffs *fs,
        const struct fffs_workload_options *options,
        struct fffs_workload_summary *summary) {
    if (!fs) {
        return FFFS_ERR_INVALID;
    }

    size_t rounds = options && options->rounds ? options->rounds : 100;
    size_t file_count = options && options->file_count ?
        options->file_count : 224;
    size_t max_file_size = options && options->max_file_size ?
        options->max_file_size : 350u * 1024u;
    if (file_count == 0 || file_count > 1000 || max_file_size == 0) {
        return FFFS_ERR_INVALID;
    }

    size_t max_alloc = max_file_size < 350u * 1024u ? max_file_size :
        350u * 1024u;
    uint8_t *buffer = malloc(max_alloc);
    uint8_t *readback = malloc(max_alloc);
    if (!buffer || !readback) {
        free(readback);
        free(buffer);
        return FFFS_ERR_NOMEM;
    }

    struct fffs_workload_summary tmp = {0};
    uint32_t rng = options ? options->seed : 1u;
    if (rng == 0) {
        rng = 1u;
    }

    size_t data_sectors = fs->sector_count > fs->index_sectors ?
        fs->sector_count - fs->index_sectors : 0;
    size_t budget = data_sectors * 2 / 3;
    size_t tiny_count = file_count < 192 ? file_count : 192;
    if (tiny_count > budget / 4) {
        tiny_count = budget / 4;
    }
    size_t medium_size = max_alloc < 50u * 1024u ? max_alloc : 50u * 1024u;
    if (medium_size < 512) {
        medium_size = max_alloc;
    }
    size_t medium_sectors = sectors_for_file(fs, medium_size);
    size_t medium_budget = budget > tiny_count ? budget - tiny_count : 0;
    size_t medium_count = medium_sectors ? medium_budget / medium_sectors / 2 :
        0;
    if (medium_count > 16) {
        medium_count = 16;
    }
    size_t used = tiny_count + medium_count * medium_sectors;
    size_t large_size = max_alloc;
    size_t large_sectors = sectors_for_file(fs, large_size);
    bool have_large = large_size >= 16u * 1024u &&
        used + large_sectors < budget;

    size_t tiny_size = max_alloc < 64 ? max_alloc : 64;
    for (size_t i = 0; i < tiny_count; i++) {
        char name[FFFS_MAX_NAME + 1];
        snprintf(name, sizeof(name), "tiny/%03zu", i);
        int err = workload_write_file(fs, name, buffer, tiny_size,
                (uint8_t)i);
        if (err != FFFS_OK) {
            free(readback);
            free(buffer);
            return err;
        }
        tmp.writes += 1;
    }
    for (size_t i = 0; i < medium_count; i++) {
        char name[FFFS_MAX_NAME + 1];
        snprintf(name, sizeof(name), "medium/%02zu", i);
        int err = workload_write_file(fs, name, buffer, medium_size,
                (uint8_t)(0x40u + i));
        if (err != FFFS_OK) {
            free(readback);
            free(buffer);
            return err;
        }
        tmp.writes += 1;
    }
    if (have_large) {
        int err = workload_write_file(fs, "large/blob", buffer, large_size,
                0xa5);
        if (err != FFFS_OK) {
            free(readback);
            free(buffer);
            return err;
        }
        tmp.writes += 1;
    }

    for (size_t round = 0; round < rounds; round++) {
        uint32_t r = next_rand(&rng);
        size_t id = tiny_count ? r % tiny_count : 0;
        char name[FFFS_MAX_NAME + 1];
        snprintf(name, sizeof(name), "tiny/%03zu", id);

        switch ((r >> 8) & 3u) {
        case 0: {
            size_t size = 16 + ((r >> 10) % 240u);
            if (size > max_alloc) {
                size = max_alloc;
            }
            uint8_t pattern = (uint8_t)(r >> 18);
            int err = workload_write_file(fs, name, buffer, size, pattern);
            if (err != FFFS_OK) {
                free(readback);
                free(buffer);
                return err;
            }
            tmp.writes += 1;
            break;
        }
        case 1: {
            int err = fffs_delete_file(fs, name);
            if (err != FFFS_OK && err != FFFS_ERR_NOT_FOUND) {
                free(readback);
                free(buffer);
                return err;
            }
            tmp.deletes += 1;
            break;
        }
        case 2: {
            if (medium_count > 0) {
                size_t mid = (r >> 16) % medium_count;
                snprintf(name, sizeof(name), "medium/%02zu", mid);
                int err = workload_read_verify(fs, name, buffer, readback,
                        medium_size, (uint8_t)(0x40u + mid));
                if (err != FFFS_OK) {
                    free(readback);
                    free(buffer);
                    return err;
                }
                tmp.reads += 1;
            }
            break;
        }
        default: {
            size_t count = 0;
            int err = fffs_list(fs, NULL, 0, &count);
            if (err != FFFS_OK) {
                free(readback);
                free(buffer);
                return err;
            }
            tmp.lists += 1;
            break;
        }
        }
    }

    if (summary) {
        *summary = tmp;
    }
    free(readback);
    free(buffer);
    return FFFS_OK;
}
