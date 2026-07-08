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
    uint16_t span_len;
    uint32_t size;
    char name[FFFS_MAX_NAME + 1];
};

enum md_state {
    MD_ERASED = 0,
    MD_VALID = 1,
    MD_TOMBSTONED = 2,
    MD_CORRUPT = 3,
    MD_UNCOMMITTED = 4,
};

static int backend_read(const struct fffs_backend *backend, size_t offset,
        void *buffer, size_t size) {
    if (offset > backend->size || size > backend->size - offset) {
        return FFFS_ERR_RANGE;
    }
    return fffs_map_backend_status(backend->read(backend->ctx, offset,
                buffer, size));
}

static bool valid_footer(const struct fffs_sector_footer *footer,
        uint32_t *serial) {
    bool live = fffs_lifecycle_is_live(footer->valid_bits,
            footer->tombstone_bits);
    bool tombstoned = footer->valid_bits != FFFS_BITMIRROR_MIXED &&
        footer->tombstone_bits == FFFS_BITMIRROR_CLEARED;
    if (footer->type != FFFS_SECTOR_TYPE_FILE || !footer->magic_valid ||
            (!live && !tombstoned)) {
        return false;
    }
    if (serial) {
        *serial = footer->serial;
    }
    return true;
}

static enum md_state decode_md(const uint8_t md[FFFS_MD_SIZE],
        size_t data_limit, size_t sector_size, struct decoded_md *out) {
    if (fffs_flash_bytes_erased(md, FFFS_MD_SIZE)) {
        return MD_ERASED;
    }
    enum fffs_bitmirror_state valid = fffs_lifecycle_valid_pair(md[0]);
    enum fffs_bitmirror_state tombstone =
        fffs_lifecycle_tombstone_pair(md[0]);
    bool uncommitted = md[0] == 0xff;
    bool live = fffs_lifecycle_is_live(valid, tombstone);
    bool tombstoned = valid != FFFS_BITMIRROR_MIXED &&
        (tombstone == FFFS_BITMIRROR_CLEARED ||
         tombstone == FFFS_BITMIRROR_MIXED);
    if (md[15] != FFFS_MD_TYPE_FILE_ROOT_V1 &&
            md[15] != FFFS_MD_TYPE_FILE_CONT_V1) {
        return MD_CORRUPT;
    }
    if (!live && !tombstoned && !uncommitted) {
        return MD_CORRUPT;
    }
    uint16_t data_off = fffs_load_le16(md + 7);
    uint16_t data_len = fffs_load_le16(md + 9);
    uint16_t span_len = fffs_load_le16(md + 5);
    (void)sector_size;
    if ((size_t)data_off + data_len > data_limit ||
            (!uncommitted && span_len == 0)) {
        return MD_CORRUPT;
    }

    if (out) {
        memset(out, 0, sizeof(*out));
        out->type = md[15];
        out->state = md[0];
        out->slot = fffs_load_le16(md + 1);
        out->data_off = data_off;
        out->data_len = data_len;
        out->next = fffs_load_le16(md + 3);
        out->span_len = span_len;
        if (md[15] == FFFS_MD_TYPE_FILE_ROOT_V1) {
            out->size = fffs_load_le32(md + 11);
        }
    }
    if (uncommitted) {
        return MD_UNCOMMITTED;
    }
    return tombstoned ? MD_TOMBSTONED : MD_VALID;
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
    *state = decode_md(md, start, summary->sector_size, decoded);
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
        uint16_t slot = fffs_load_le16(rec);
        uint16_t head = fffs_load_le16(rec + 2);
        if (slot == UINT16_MAX && head == UINT16_MAX) {
            return FFFS_OK;
        }
        if (slot == 0 && head == 0) {
            continue;
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
        uint16_t slot = fffs_load_le16(rec);
        uint16_t head = fffs_load_le16(rec + 2);
        if (slot == UINT16_MAX && head == UINT16_MAX) {
            break;
        }
        if (slot == 0 && head == 0) {
            continue;
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
    struct fffs_sector_footer view;
    fffs_decode_sector_footer(footer, &view);
    if (!valid_footer(&view, NULL)) {
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
        if (*state == MD_TOMBSTONED || *state == MD_UNCOMMITTED ||
                decoded->slot != want_slot) {
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
        if (sector >= current &&
                sector < (size_t)current + decoded.span_len) {
            *reachable = true;
            return FFFS_OK;
        }
        current = decoded.next;
    }
    return FFFS_OK;
}

static int mark_live_chains(const struct fffs_backend *backend,
        struct fffs_inspect_summary *summary, const uint16_t *live_heads,
        bool *reachable, bool *span_heads, bool *seen_root) {
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

            span_heads[current] = true;
            for (uint16_t i = 0; i < decoded.span_len; i++) {
                size_t owned = (size_t)current + i;
                if (owned >= summary->sector_count) {
                    corrupt = true;
                    break;
                }
                reachable[owned] = true;
            }
            if (corrupt) {
                break;
            }
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
        struct fffs_inspect_summary *summary, const uint16_t *live_heads,
        const bool *reachable, const bool *span_heads) {
    for (size_t sector = summary->index_sectors;
            sector < summary->sector_count; sector++) {
        uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
        size_t footer_off = sector * summary->sector_size +
            summary->sector_size - FFFS_SECTOR_FOOTER_SIZE;
        int err = backend_read(backend, footer_off, footer, sizeof(footer));
        if (err != FFFS_OK) {
            return err;
        }

        struct fffs_sector_footer view;
        fffs_decode_sector_footer(footer, &view);
        if (view.erased) {
            summary->data_sectors_erased += 1;
            continue;
        }
        if (!valid_footer(&view, NULL)) {
            continue;
        }
        if (view.tombstone_bits == FFFS_BITMIRROR_CLEARED) {
            summary->data_sectors_tombstoned += 1;
            summary->md_tombstoned += 1;
            continue;
        }
        summary->data_sectors_owned += 1;
        if (reachable[sector] && !span_heads[sector]) {
            continue;
        }

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
                summary->md_tombstoned += 1;
                if (cursor <= claimed_data_end) {
                    break;
                }
                continue;
            }
            if (state == MD_UNCOMMITTED) {
                summary->md_obsolete_orphaned += 1;
                if (cursor <= claimed_data_end) {
                    break;
                }
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
    bool *span_heads = calloc(tmp.sector_count, sizeof(*span_heads));
    if (!live_heads || !seen_root || !reachable || !span_heads) {
        free(span_heads);
        free(reachable);
        free(seen_root);
        free(live_heads);
        return FFFS_ERR_NOMEM;
    }

    err = inspect_index(backend, &tmp, live_heads);
    if (err == FFFS_OK) {
        err = mark_live_chains(backend, &tmp, live_heads, reachable,
                span_heads,
                seen_root);
    }
    if (err == FFFS_OK) {
        err = inspect_data_sectors(backend, &tmp, live_heads, reachable,
                span_heads);
    }

    free(span_heads);
    free(reachable);
    free(seen_root);
    free(live_heads);
    if (err != FFFS_OK) {
        return err;
    }
    *summary = tmp;
    return FFFS_OK;
}

static void livemap_mark(uint8_t *page_state, size_t page_count,
        size_t page_size, size_t base, size_t len, uint8_t val) {
    if (len == 0) {
        return;
    }
    size_t p0 = base / page_size;
    size_t p1 = (base + len - 1) / page_size;
    for (size_t p = p0; p <= p1 && p < page_count; p++) {
        if (val > page_state[p]) {
            page_state[p] = val;
        }
    }
}

int fffs_inspect_live_map(const struct fffs_backend *backend,
        uint8_t *page_state, size_t page_count, size_t page_size) {
    if (!backend || !page_state || !page_size) {
        return FFFS_ERR_INVALID;
    }
    struct fffs_inspect_summary tmp;
    int err = discover(backend, &tmp);
    if (err != FFFS_OK) {
        return err;
    }
    memset(page_state, 0, page_count);

    uint16_t *live_heads = calloc(FFFS_SLOT_COUNT, sizeof(*live_heads));
    bool *seen_root = calloc(FFFS_SLOT_COUNT, sizeof(*seen_root));
    bool *reachable = calloc(tmp.sector_count, sizeof(*reachable));
    bool *span_heads = calloc(tmp.sector_count, sizeof(*span_heads));
    if (!live_heads || !seen_root || !reachable || !span_heads) {
        free(span_heads); free(reachable); free(seen_root); free(live_heads);
        return FFFS_ERR_NOMEM;
    }

    err = inspect_index(backend, &tmp, live_heads);
    if (err == FFFS_OK) {
        err = mark_live_chains(backend, &tmp, live_heads, reachable,
                span_heads, seen_root);
    }
    if (err != FFFS_OK) {
        goto done;
    }

    /* index area holds the namespace records — metadata */
    for (size_t s = 0; s < tmp.index_sectors; s++) {
        livemap_mark(page_state, page_count, page_size,
                s * tmp.sector_size, tmp.sector_size, 1);
    }

    for (size_t sector = tmp.index_sectors; sector < tmp.sector_count; sector++) {
        uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
        size_t footer_off = sector * tmp.sector_size +
            tmp.sector_size - FFFS_SECTOR_FOOTER_SIZE;
        err = backend_read(backend, footer_off, footer, sizeof(footer));
        if (err != FFFS_OK) {
            goto done;
        }
        struct fffs_sector_footer view;
        fffs_decode_sector_footer(footer, &view);
        if (view.erased) {
            continue;                                  /* blank sector */
        }
        if (!valid_footer(&view, NULL)) {
            continue;                                  /* corrupt / non-file */
        }
        if (view.tombstone_bits == FFFS_BITMIRROR_CLEARED) {
            livemap_mark(page_state, page_count, page_size,
                    sector * tmp.sector_size, tmp.sector_size, 2); /* dead sector */
            continue;
        }

        /* owned sector: footer is live metadata, then walk its records */
        livemap_mark(page_state, page_count, page_size, footer_off,
                FFFS_SECTOR_FOOTER_SIZE, 1);
        size_t cursor = tmp.sector_size - FFFS_SECTOR_FOOTER_SIZE;
        size_t claimed_data_end = 0;
        while (cursor > FFFS_SECTOR_FOOTER_SIZE) {
            struct decoded_md decoded;
            enum md_state state;
            size_t record_start = 0;
            err = read_record_before(backend, &tmp, sector, cursor,
                    &decoded, &state, &record_start);
            if (err != FFFS_OK) {
                goto done;
            }
            if (state == MD_ERASED || state == MD_CORRUPT) {
                break;
            }
            livemap_mark(page_state, page_count, page_size,
                    sector * tmp.sector_size + record_start,
                    cursor - record_start, 1);          /* md record: metadata */
            cursor = record_start;
            size_t data_end = (size_t)decoded.data_off + decoded.data_len;
            if (data_end > claimed_data_end) {
                claimed_data_end = data_end;
            }
            uint8_t dval;
            if (state == MD_TOMBSTONED || state == MD_UNCOMMITTED) {
                dval = 2;
            } else {
                bool live = false;
                err = record_reachable_for_slot(backend, &tmp, live_heads,
                        decoded.slot, sector, &live);
                if (err != FFFS_OK) {
                    goto done;
                }
                dval = live ? 3 : 2;
            }
            livemap_mark(page_state, page_count, page_size,
                    sector * tmp.sector_size + decoded.data_off,
                    decoded.data_len, dval);
            if (cursor <= claimed_data_end) {
                break;
            }
        }
    }

done:
    free(span_heads); free(reachable); free(seen_root); free(live_heads);
    return err;
}

enum frag_pin_class {
    FRAG_ROOT_ONLY = 0,
    FRAG_CONTINUATION_ONLY = 1,
    FRAG_MIXED = 2,
    FRAG_NO_LIVE = 3,
    FRAG_CLASS_COUNT = 4,
};

struct frag_class_summary {
    size_t sectors;
    size_t reclaimable_bytes;
    size_t live_bytes;
    size_t immediate_free_bytes;
};

struct fragstats_summary {
    size_t data_sectors;
    size_t erased_sectors;
    size_t owned_sectors;
    size_t tombstoned_sectors;
    size_t corrupt_sectors;
    size_t span_tail_sectors;
    size_t full_hint_sectors;
    size_t immediate_free_sectors;
    size_t immediate_free_bytes;
    struct frag_class_summary class_summary[FRAG_CLASS_COUNT];
};

static void mark_range(bool *mask, size_t mask_size, size_t off, size_t len) {
    if (off > mask_size) {
        return;
    }
    if (len > mask_size - off) {
        len = mask_size - off;
    }
    for (size_t i = 0; i < len; i++) {
        mask[off + i] = true;
    }
}

static size_t count_marked(const bool *mask, size_t size) {
    size_t count = 0;
    for (size_t i = 0; i < size; i++) {
        if (mask[i]) {
            count++;
        }
    }
    return count;
}

static int range_erased(const struct fffs_backend *backend, size_t off,
        size_t len, bool *erased) {
    *erased = true;
    uint8_t buf[64];
    while (len > 0) {
        size_t n = len < sizeof(buf) ? len : sizeof(buf);
        int err = backend_read(backend, off, buf, n);
        if (err != FFFS_OK) {
            return err;
        }
        for (size_t i = 0; i < n; i++) {
            if (buf[i] != 0xff) {
                *erased = false;
                return FFFS_OK;
            }
        }
        off += n;
        len -= n;
    }
    return FFFS_OK;
}

static enum frag_pin_class pin_class_for_sector(bool has_root,
        bool has_continuation) {
    if (has_root && has_continuation) {
        return FRAG_MIXED;
    }
    if (has_continuation) {
        return FRAG_CONTINUATION_ONLY;
    }
    if (has_root) {
        return FRAG_ROOT_ONLY;
    }
    return FRAG_NO_LIVE;
}

static const char *frag_class_name(enum frag_pin_class cls) {
    switch (cls) {
    case FRAG_ROOT_ONLY:
        return "root-only";
    case FRAG_CONTINUATION_ONLY:
        return "continuation-only";
    case FRAG_MIXED:
        return "mixed";
    case FRAG_NO_LIVE:
        return "erase-only";
    default:
        return "unknown";
    }
}

static int inspect_fragstats_sector(const struct fffs_backend *backend,
        const struct fffs_inspect_summary *summary,
        const uint16_t *live_heads, const bool *reachable,
        const bool *span_heads, size_t sector,
        struct fragstats_summary *stats, bool *live_mask,
        bool *free_mask) {
    uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
    size_t footer_off = sector * summary->sector_size +
        summary->sector_size - FFFS_SECTOR_FOOTER_SIZE;
    int err = backend_read(backend, footer_off, footer, sizeof(footer));
    if (err != FFFS_OK) {
        return err;
    }

    stats->data_sectors++;
    struct fffs_sector_footer view;
    fffs_decode_sector_footer(footer, &view);
    if (view.erased) {
        stats->erased_sectors++;
        return FFFS_OK;
    }

    uint32_t serial = 0;
    if (!valid_footer(&view, &serial)) {
        (void)serial;
        stats->corrupt_sectors++;
        return FFFS_OK;
    }
    if (view.tombstone_bits == FFFS_BITMIRROR_CLEARED) {
        stats->tombstoned_sectors++;
        return FFFS_OK;
    }

    stats->owned_sectors++;
    if (view.full_bits == FFFS_BITMIRROR_CLEARED) {
        stats->full_hint_sectors++;
    }
    bool span_tail = reachable[sector] && !span_heads[sector];
    if (span_tail) {
        stats->span_tail_sectors++;
    }

    memset(live_mask, 0, summary->sector_size * sizeof(*live_mask));
    memset(free_mask, 0, summary->sector_size * sizeof(*free_mask));
    mark_range(live_mask, summary->sector_size,
            summary->sector_size - FFFS_SECTOR_FOOTER_SIZE,
            FFFS_SECTOR_FOOTER_SIZE);

    size_t cursor = summary->sector_size - FFFS_SECTOR_FOOTER_SIZE;
    size_t claimed_data_end = 0;
    size_t metadata_start = cursor;
    bool has_root = false;
    bool has_continuation = false;
    bool parsed_live_record = false;
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
            stats->corrupt_sectors++;
            break;
        }

        cursor = record_start;
        metadata_start = record_start;
        size_t data_end = (size_t)decoded.data_off + decoded.data_len;
        if (data_end > claimed_data_end) {
            claimed_data_end = data_end;
        }

        bool record_live = false;
        if (state == MD_VALID) {
            err = record_reachable_for_slot(backend, summary, live_heads,
                    decoded.slot, sector, &record_live);
            if (err != FFFS_OK) {
                return err;
            }
        }
        if (record_live) {
            parsed_live_record = true;
            mark_range(live_mask, summary->sector_size, decoded.data_off,
                    decoded.data_len);
            mark_range(live_mask, summary->sector_size, record_start,
                    FFFS_MD_FILE_RECORD_SIZE);
            if (decoded.type == FFFS_MD_TYPE_FILE_ROOT_V1) {
                has_root = true;
            } else {
                has_continuation = true;
            }
        }

        if (cursor <= claimed_data_end) {
            break;
        }
    }

    if (span_tail) {
        has_continuation = true;
        if (!parsed_live_record) {
            mark_range(live_mask, summary->sector_size, 0,
                    summary->sector_size - FFFS_SECTOR_FOOTER_SIZE);
        }
    }

    size_t immediate_free = 0;
    if (metadata_start > claimed_data_end) {
        bool erased = false;
        err = range_erased(backend, sector * summary->sector_size +
                claimed_data_end, metadata_start - claimed_data_end,
                &erased);
        if (err != FFFS_OK) {
            return err;
        }
        if (erased) {
            mark_range(free_mask, summary->sector_size, claimed_data_end,
                    metadata_start - claimed_data_end);
            immediate_free = metadata_start - claimed_data_end;
        }
    }
    if (immediate_free != 0) {
        stats->immediate_free_sectors++;
        stats->immediate_free_bytes += immediate_free;
    }

    size_t live_bytes = count_marked(live_mask, summary->sector_size);
    size_t free_bytes = count_marked(free_mask, summary->sector_size);
    size_t reclaimable = 0;
    for (size_t i = 0; i < summary->sector_size; i++) {
        if (!live_mask[i] && !free_mask[i]) {
            reclaimable++;
        }
    }

    enum frag_pin_class cls = pin_class_for_sector(has_root,
            has_continuation);
    struct frag_class_summary *class_summary = &stats->class_summary[cls];
    class_summary->sectors++;
    class_summary->reclaimable_bytes += reclaimable;
    class_summary->live_bytes += live_bytes;
    class_summary->immediate_free_bytes += free_bytes;
    return FFFS_OK;
}

int fffs_inspect_fragstats_dump(const struct fffs_backend *backend,
        FILE *out) {
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
    bool *span_heads = calloc(summary.sector_count, sizeof(*span_heads));
    bool *live_mask = calloc(summary.sector_size, sizeof(*live_mask));
    bool *free_mask = calloc(summary.sector_size, sizeof(*free_mask));
    if (!live_heads || !seen_root || !reachable || !span_heads ||
            !live_mask || !free_mask) {
        free(free_mask);
        free(live_mask);
        free(span_heads);
        free(reachable);
        free(seen_root);
        free(live_heads);
        return FFFS_ERR_NOMEM;
    }

    err = replay_active_index(backend, &summary, live_heads);
    if (err == FFFS_OK) {
        err = mark_live_chains(backend, &summary, live_heads, reachable,
                span_heads, seen_root);
    }

    struct fragstats_summary stats = {0};
    for (size_t sector = summary.index_sectors;
            err == FFFS_OK && sector < summary.sector_count; sector++) {
        err = inspect_fragstats_sector(backend, &summary, live_heads,
                reachable, span_heads, sector, &stats, live_mask, free_mask);
    }

    if (err == FFFS_OK) {
        fprintf(out, "FASTFFS fragstats: sector_size=%zu sector_count=%zu "
                "index_sectors=%u active_index=%zu serial=%u\n",
                summary.sector_size, summary.sector_count,
                (unsigned)summary.index_sectors,
                summary.active_index_sector,
                (unsigned)summary.active_index_serial);
        fprintf(out, "sectors: data=%zu owned=%zu erased=%zu "
                "tombstoned=%zu corrupt=%zu span_tail=%zu full_hint=%zu\n",
                stats.data_sectors, stats.owned_sectors,
                stats.erased_sectors, stats.tombstoned_sectors,
                stats.corrupt_sectors, stats.span_tail_sectors,
                stats.full_hint_sectors);
        fprintf(out, "immediate_free: sectors=%zu bytes=%zu\n",
                stats.immediate_free_sectors, stats.immediate_free_bytes);
        for (size_t i = 0; i < FRAG_CLASS_COUNT; i++) {
            const struct frag_class_summary *class_summary =
                &stats.class_summary[i];
            fprintf(out, "class=%s sectors=%zu reclaimable_bytes=%zu "
                    "live_bytes=%zu immediate_free_bytes=%zu\n",
                    frag_class_name((enum frag_pin_class)i),
                    class_summary->sectors,
                    class_summary->reclaimable_bytes,
                    class_summary->live_bytes,
                    class_summary->immediate_free_bytes);
        }
    }

    free(free_mask);
    free(live_mask);
    free(span_heads);
    free(reachable);
    free(seen_root);
    free(live_heads);
    return err;
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
    case MD_UNCOMMITTED:
        return "uncommitted";
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
        } else if (fffs_flash_bytes_erased(hdr, sizeof(hdr))) {
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
        uint16_t slot = fffs_load_le16(rec);
        uint16_t head = fffs_load_le16(rec + 2);
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
        const uint16_t *live_heads, const bool *reachable,
        const bool *span_heads) {
    for (size_t sector = summary->index_sectors;
            sector < summary->sector_count; sector++) {
        uint8_t footer[FFFS_SECTOR_FOOTER_SIZE];
        size_t footer_off = sector * summary->sector_size +
            summary->sector_size - FFFS_SECTOR_FOOTER_SIZE;
        int err = backend_read(backend, footer_off, footer, sizeof(footer));
        if (err != FFFS_OK) {
            return err;
        }
        struct fffs_sector_footer view;
        fffs_decode_sector_footer(footer, &view);
        if (view.erased) {
            continue;
        }

        uint32_t serial = 0;
        if (!valid_footer(&view, &serial)) {
            fprintf(out, "sector=%zu invalid-footer\n", sector);
            continue;
        }

        size_t cursor = summary->sector_size - FFFS_SECTOR_FOOTER_SIZE;
        size_t claimed_data_end = 0;
        bool printed_any = false;
        if (reachable[sector] && !span_heads[sector]) {
            fprintf(out, "sector=%zu serial=%" PRIu32 " md=span-tail\n",
                    sector, serial);
            continue;
        }
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
            if (state == MD_VALID || state == MD_TOMBSTONED ||
                    state == MD_UNCOMMITTED) {
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
    bool *span_heads = calloc(summary.sector_count, sizeof(*span_heads));
    if (!live_heads || !seen_root || !reachable || !span_heads) {
        free(span_heads);
        free(reachable);
        free(seen_root);
        free(live_heads);
        return FFFS_ERR_NOMEM;
    }
    err = replay_active_index(backend, &summary, live_heads);
    if (err != FFFS_OK) {
        free(span_heads);
        free(reachable);
        free(seen_root);
        free(live_heads);
        return err;
    }
    err = mark_live_chains(backend, &summary, live_heads, reachable,
            span_heads,
            seen_root);
    if (err != FFFS_OK) {
        free(span_heads);
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
        err = dump_data_sectors(backend, out, &summary, live_heads,
                reachable, span_heads);
    }

    free(span_heads);
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
    err = fffs_write(&file, buffer, size);
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
