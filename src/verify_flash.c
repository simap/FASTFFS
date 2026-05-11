#include "fastffs/verify_flash.h"

#include "bd/lfs_emubd.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

struct staged_mutation {
    bool active;
    size_t offset;
    size_t size;
    uint8_t *data;
};

struct ffsv_flash {
    struct ffsv_flash_config cfg;
    struct lfs_config lfs_cfg;
    struct lfs_emubd_config emu_cfg;
    lfs_emubd_t emu;
    uint8_t *image_cache;
    uint64_t next_sequence;
    uint64_t time_ns;
    struct ffsv_failure_injection failure;
    struct ffsv_op_counts counts[FFSV_OP_COUNT];
    struct ffsv_sector_counts *sector_counts;
    size_t sector_count;
    struct ffsv_op_record *log;
    size_t log_count;
    size_t log_capacity;
    struct staged_mutation staged;
};

static int checked_config(const struct ffsv_flash_config *cfg) {
    if (!cfg || cfg->total_size == 0 || cfg->sector_size == 0 ||
            cfg->program_granule == 0 || cfg->read_granule == 0) {
        return FFSV_ERR_INVALID;
    }
    if (cfg->total_size % cfg->sector_size != 0 ||
            cfg->sector_size % cfg->program_granule != 0 ||
            cfg->sector_size % cfg->read_granule != 0) {
        return FFSV_ERR_INVALID;
    }
    return FFSV_OK;
}

int ffsv_flash_config_preset(struct ffsv_flash_config *cfg,
        enum ffsv_flash_preset preset, size_t total_size) {
    if (!cfg || total_size == 0) {
        return FFSV_ERR_INVALID;
    }

    *cfg = (struct ffsv_flash_config){
        .total_size = total_size,
        .sector_size = 4096,
        .program_granule = 4,
        .read_granule = 1,
        .erased_value = 0xff,
        .erase_cycles = 100000,
        .timing = {
            .read_fixed_ns = 500,
            .read_per_byte_ns = 20,
            .program_fixed_ns = 8000,
            .program_per_byte_ns = 180,
            .erase_fixed_ns = 45000000,
            .erase_per_byte_ns = 0,
            .blank_check_fixed_ns = 500,
            .blank_check_per_byte_ns = 20,
        },
        .max_log_entries = 4096,
    };

    switch (preset) {
    case FFSV_PRESET_GENERIC_NOR:
        break;
    case FFSV_PRESET_ESP32S3_QIO:
        cfg->program_granule = 16;
        cfg->timing.read_fixed_ns = 300;
        cfg->timing.read_per_byte_ns = 8;
        cfg->timing.program_fixed_ns = 12000;
        cfg->timing.program_per_byte_ns = 120;
        cfg->timing.erase_fixed_ns = 50000000;
        break;
    case FFSV_PRESET_SMALL_SPI_NOR:
        cfg->program_granule = 1;
        cfg->timing.read_fixed_ns = 1500;
        cfg->timing.read_per_byte_ns = 80;
        cfg->timing.program_fixed_ns = 12000;
        cfg->timing.program_per_byte_ns = 1500;
        cfg->timing.erase_fixed_ns = 30000000;
        break;
    default:
        return FFSV_ERR_INVALID;
    }

    return checked_config(cfg);
}

static int check_range(const struct ffsv_flash *flash,
        size_t offset, size_t size) {
    if (offset > flash->cfg.total_size ||
            size > flash->cfg.total_size - offset) {
        return FFSV_ERR_RANGE;
    }
    return FFSV_OK;
}

static int check_alignment(size_t offset, size_t size, size_t granule) {
    if ((offset % granule) != 0 || (size % granule) != 0) {
        return FFSV_ERR_ALIGNMENT;
    }
    return FFSV_OK;
}

static size_t sector_of(const struct ffsv_flash *flash, size_t offset) {
    if (offset >= flash->cfg.total_size) {
        return flash->sector_count;
    }
    return offset / flash->cfg.sector_size;
}

static bool op_matches(const struct ffsv_flash *flash,
        enum ffsv_op_type type, uint64_t sequence) {
    return flash->failure.enabled &&
        flash->failure.sequence == sequence &&
        ((flash->failure.op_mask & (UINT32_C(1) << type)) != 0);
}

static uint64_t duration_for(const struct ffsv_flash *flash,
        enum ffsv_op_type type, size_t size) {
    const struct ffsv_timing *t = &flash->cfg.timing;
    switch (type) {
    case FFSV_OP_READ:
        return t->read_fixed_ns + t->read_per_byte_ns * size;
    case FFSV_OP_PROGRAM:
    case FFSV_OP_STAGE_PROGRAM:
    case FFSV_OP_COMMIT_STAGED:
        return t->program_fixed_ns + t->program_per_byte_ns * size;
    case FFSV_OP_ERASE:
        return t->erase_fixed_ns + t->erase_per_byte_ns * size;
    case FFSV_OP_BLANK_CHECK:
        return t->blank_check_fixed_ns + t->blank_check_per_byte_ns * size;
    default:
        return 0;
    }
}

static void add_sector_counts(struct ffsv_flash *flash,
        enum ffsv_op_type type, size_t offset, size_t size) {
    if (size == 0 || offset >= flash->cfg.total_size) {
        return;
    }

    size_t first = offset / flash->cfg.sector_size;
    size_t last = (offset + size - 1) / flash->cfg.sector_size;
    for (size_t sector = first; sector <= last; sector++) {
        size_t sector_start = sector * flash->cfg.sector_size;
        size_t sector_end = sector_start + flash->cfg.sector_size;
        size_t start = offset > sector_start ? offset : sector_start;
        size_t end = offset + size < sector_end ? offset + size : sector_end;
        size_t n = end - start;

        switch (type) {
        case FFSV_OP_READ:
            flash->sector_counts[sector].read_calls += 1;
            flash->sector_counts[sector].read_bytes += n;
            break;
        case FFSV_OP_PROGRAM:
        case FFSV_OP_STAGE_PROGRAM:
        case FFSV_OP_COMMIT_STAGED:
            flash->sector_counts[sector].programmed_bytes += n;
            break;
        case FFSV_OP_ERASE:
            flash->sector_counts[sector].erased_bytes += n;
            break;
        case FFSV_OP_BLANK_CHECK:
            flash->sector_counts[sector].blank_checked_bytes += n;
            break;
        default:
            break;
        }
    }
}

static struct ffsv_op_record *begin_op(struct ffsv_flash *flash,
        enum ffsv_op_type type, size_t offset, size_t size,
        const char *call_site, bool *inject) {
    uint64_t seq = flash->next_sequence++;
    *inject = op_matches(flash, type, seq);
    flash->counts[type].calls += 1;
    flash->counts[type].bytes += size;
    add_sector_counts(flash, type, offset, size);

    if (flash->log_count >= flash->log_capacity) {
        return NULL;
    }

    struct ffsv_op_record *rec = &flash->log[flash->log_count++];
    *rec = (struct ffsv_op_record){
        .sequence = seq,
        .type = type,
        .offset = offset,
        .size = size,
        .sector = sector_of(flash, offset),
        .call_site = call_site,
        .time_before_ns = flash->time_ns,
        .time_after_ns = flash->time_ns,
        .result = FFSV_OK,
        .injected = *inject,
        .injected_phase = *inject ? flash->failure.phase : FFSV_FAIL_BEFORE,
        .committed_bytes = 0,
    };
    return rec;
}

static int finish_op(struct ffsv_flash *flash, struct ffsv_op_record *rec,
        enum ffsv_op_type type, size_t size, int result,
        size_t committed_bytes) {
    flash->time_ns += duration_for(flash, type, size);
    if (rec) {
        rec->time_after_ns = flash->time_ns;
        rec->result = result;
        rec->committed_bytes = committed_bytes;
    }
    return result;
}

static int injected_status(const struct ffsv_flash *flash) {
    return flash->failure.status ? flash->failure.status : FFSV_ERR_INJECTED;
}

static size_t injected_partial(const struct ffsv_flash *flash, size_t size,
        size_t granule) {
    size_t partial = flash->failure.partial_bytes ?
        flash->failure.partial_bytes : size / 2;
    if (granule > 1) {
        partial -= partial % granule;
    }
    if (partial == 0 && size >= granule) {
        partial = granule;
    }
    if (partial > size) {
        partial = size;
    }
    return partial;
}

static int validate_program_transition(const struct ffsv_flash *flash,
        size_t offset, const uint8_t *data, size_t size) {
    const uint8_t *image = ffsv_flash_image((struct ffsv_flash *)flash);
    if (!image) {
        return FFSV_ERR_INVALID;
    }
    for (size_t i = 0; i < size; i++) {
        if ((uint8_t)(image[offset + i] & data[i]) != data[i]) {
            return FFSV_ERR_PROGRAM_TRANSITION;
        }
    }
    return FFSV_OK;
}

static int map_lfs_error(int err) {
    return err == 0 ? FFSV_OK : FFSV_ERR_IO;
}

static void refresh_image_cache(struct ffsv_flash *flash) {
    for (size_t block = 0; block < flash->sector_count; block++) {
        uint8_t *dst = flash->image_cache + block * flash->cfg.sector_size;
        const lfs_emubd_block_t *src = flash->emu.blocks[block];
        if (src) {
            memcpy(dst, src->data, flash->cfg.sector_size);
        } else {
            memset(dst, flash->cfg.erased_value, flash->cfg.sector_size);
        }
    }
}

static int raw_erase_prefix(struct ffsv_flash *flash,
        size_t offset, size_t size) {
    size_t remaining = size;
    while (remaining > 0) {
        size_t sector = offset / flash->cfg.sector_size;
        size_t in_sector = offset % flash->cfg.sector_size;
        size_t n = flash->cfg.sector_size - in_sector;
        if (n > remaining) {
            n = remaining;
        }

        lfs_emubd_wear_t wear = (lfs_emubd_wear_t)lfs_emubd_wear(
                &flash->lfs_cfg, (lfs_block_t)sector);
        int err = lfs_emubd_setwear(&flash->lfs_cfg, (lfs_block_t)sector,
                wear);
        if (err) {
            return map_lfs_error(err);
        }
        memset(flash->emu.blocks[sector]->data + in_sector,
                flash->cfg.erased_value, n);

        offset += n;
        remaining -= n;
    }
    refresh_image_cache(flash);
    return FFSV_OK;
}

static int emu_program_range(struct ffsv_flash *flash, size_t offset,
        const uint8_t *data, size_t size) {
    size_t data_offset = 0;
    while (size > 0) {
        size_t block = offset / flash->cfg.sector_size;
        size_t off = offset % flash->cfg.sector_size;
        size_t n = flash->cfg.sector_size - off;
        if (n > size) {
            n = size;
        }

        int err = lfs_emubd_prog(&flash->lfs_cfg, (lfs_block_t)block,
                (lfs_off_t)off, data + data_offset, (lfs_size_t)n);
        if (err) {
            return map_lfs_error(err);
        }

        offset += n;
        data_offset += n;
        size -= n;
    }
    refresh_image_cache(flash);
    return FFSV_OK;
}

static int raw_corrupt(struct ffsv_flash *flash, size_t offset,
        const uint8_t *data, size_t size, bool xor_data) {
    size_t remaining = size;
    size_t data_offset = 0;
    while (remaining > 0) {
        size_t sector = offset / flash->cfg.sector_size;
        size_t in_sector = offset % flash->cfg.sector_size;
        size_t n = flash->cfg.sector_size - in_sector;
        if (n > remaining) {
            n = remaining;
        }

        lfs_emubd_wear_t wear = (lfs_emubd_wear_t)lfs_emubd_wear(
                &flash->lfs_cfg, (lfs_block_t)sector);
        int err = lfs_emubd_setwear(&flash->lfs_cfg, (lfs_block_t)sector,
                wear);
        if (err) {
            return map_lfs_error(err);
        }
        uint8_t *dst = flash->emu.blocks[sector]->data + in_sector;
        for (size_t i = 0; i < n; i++) {
            dst[i] = xor_data ? (uint8_t)(dst[i] ^ data[data_offset + i])
                : data[data_offset + i];
        }

        offset += n;
        data_offset += n;
        remaining -= n;
    }
    refresh_image_cache(flash);
    return FFSV_OK;
}

static int raw_load_image(struct ffsv_flash *flash, const uint8_t *image,
        size_t size, const uint32_t *wear) {
    if (size != flash->cfg.total_size) {
        return FFSV_ERR_INVALID;
    }

    for (size_t sector = 0; sector < flash->sector_count; sector++) {
        int err = lfs_emubd_setwear(&flash->lfs_cfg, (lfs_block_t)sector,
                wear ? (lfs_emubd_wear_t)wear[sector] :
                    (lfs_emubd_wear_t)lfs_emubd_wear(&flash->lfs_cfg,
                        (lfs_block_t)sector));
        if (err) {
            return map_lfs_error(err);
        }
        memcpy(flash->emu.blocks[sector]->data,
                image + sector * flash->cfg.sector_size,
                flash->cfg.sector_size);
    }
    refresh_image_cache(flash);
    return FFSV_OK;
}

int ffsv_flash_create(struct ffsv_flash **out,
        const struct ffsv_flash_config *cfg) {
    if (!out) {
        return FFSV_ERR_INVALID;
    }
    int err = checked_config(cfg);
    if (err) {
        return err;
    }

    struct ffsv_flash *flash = calloc(1, sizeof(*flash));
    if (!flash) {
        return FFSV_ERR_NOMEM;
    }

    flash->cfg = *cfg;
    flash->sector_count = cfg->total_size / cfg->sector_size;
    flash->log_capacity = cfg->max_log_entries ? cfg->max_log_entries : 4096;
    flash->image_cache = malloc(cfg->total_size);
    flash->sector_counts = calloc(flash->sector_count,
            sizeof(*flash->sector_counts));
    flash->log = calloc(flash->log_capacity, sizeof(*flash->log));
    if (!flash->image_cache || !flash->sector_counts || !flash->log) {
        ffsv_flash_destroy(flash);
        return FFSV_ERR_NOMEM;
    }

    flash->lfs_cfg = (struct lfs_config){
        .context = &flash->emu,
        .read = lfs_emubd_read,
        .prog = lfs_emubd_prog,
        .erase = lfs_emubd_erase,
        .sync = lfs_emubd_sync,
        .read_size = (lfs_size_t)cfg->read_granule,
        .prog_size = (lfs_size_t)cfg->program_granule,
        .block_size = (lfs_size_t)cfg->sector_size,
        .block_count = (lfs_size_t)flash->sector_count,
    };
    flash->emu_cfg = (struct lfs_emubd_config){
        .read_size = (lfs_size_t)cfg->read_granule,
        .prog_size = (lfs_size_t)cfg->program_granule,
        .erase_size = (lfs_size_t)cfg->sector_size,
        .erase_count = (lfs_size_t)flash->sector_count,
        .erase_value = cfg->erased_value,
        .erase_cycles = cfg->erase_cycles,
        .badblock_behavior = LFS_EMUBD_BADBLOCK_ERASEERROR,
        .power_cycles = 0,
        .powerloss_behavior = LFS_EMUBD_POWERLOSS_NOOP,
    };

    err = lfs_emubd_create(&flash->lfs_cfg, &flash->emu_cfg);
    if (err) {
        ffsv_flash_destroy(flash);
        return map_lfs_error(err);
    }
    refresh_image_cache(flash);
    *out = flash;
    return FFSV_OK;
}

int ffsv_flash_create_with_preset(struct ffsv_flash **out,
        enum ffsv_flash_preset preset, size_t total_size) {
    struct ffsv_flash_config cfg;
    int err = ffsv_flash_config_preset(&cfg, preset, total_size);
    if (err) {
        return err;
    }
    return ffsv_flash_create(out, &cfg);
}

void ffsv_flash_destroy(struct ffsv_flash *flash) {
    if (!flash) {
        return;
    }
    if (flash->emu.blocks) {
        lfs_emubd_destroy(&flash->lfs_cfg);
    }
    free(flash->staged.data);
    free(flash->log);
    free(flash->sector_counts);
    free(flash->image_cache);
    free(flash);
}

int ffsv_flash_read(struct ffsv_flash *flash, size_t offset,
        void *buffer, size_t size, const char *call_site) {
    if (!flash || (!buffer && size)) {
        return FFSV_ERR_INVALID;
    }
    int err = check_range(flash, offset, size);
    if (!err) {
        err = check_alignment(offset, size, flash->cfg.read_granule);
    }

    size_t original_size = size;
    bool inject = false;
    struct ffsv_op_record *rec = begin_op(flash, FFSV_OP_READ,
            offset, size, call_site, &inject);
    if (err) {
        return finish_op(flash, rec, FFSV_OP_READ, size, err, 0);
    }
    if (inject && flash->failure.phase == FFSV_FAIL_BEFORE) {
        return finish_op(flash, rec, FFSV_OP_READ, size,
                injected_status(flash), 0);
    }

    while (size > 0) {
        size_t block = offset / flash->cfg.sector_size;
        size_t off = offset % flash->cfg.sector_size;
        size_t n = flash->cfg.sector_size - off;
        if (n > size) {
            n = size;
        }
        err = lfs_emubd_read(&flash->lfs_cfg, (lfs_block_t)block,
                (lfs_off_t)off, buffer, (lfs_size_t)n);
        if (err) {
            return finish_op(flash, rec, FFSV_OP_READ, original_size,
                    map_lfs_error(err), 0);
        }
        offset += n;
        buffer = (uint8_t *)buffer + n;
        size -= n;
    }

    int result = inject ? injected_status(flash) : FFSV_OK;
    return finish_op(flash, rec, FFSV_OP_READ, original_size,
            result, 0);
}

int ffsv_flash_blank_check(struct ffsv_flash *flash, size_t offset,
        size_t size, const char *call_site) {
    if (!flash) {
        return FFSV_ERR_INVALID;
    }
    int err = check_range(flash, offset, size);
    if (!err) {
        err = check_alignment(offset, size, flash->cfg.read_granule);
    }

    bool inject = false;
    struct ffsv_op_record *rec = begin_op(flash, FFSV_OP_BLANK_CHECK,
            offset, size, call_site, &inject);
    if (err) {
        return finish_op(flash, rec, FFSV_OP_BLANK_CHECK, size, err, 0);
    }
    if (inject && flash->failure.phase == FFSV_FAIL_BEFORE) {
        return finish_op(flash, rec, FFSV_OP_BLANK_CHECK, size,
                injected_status(flash), 0);
    }

    const uint8_t *image = ffsv_flash_image(flash);
    for (size_t i = 0; i < size; i++) {
        if (image[offset + i] != flash->cfg.erased_value) {
            return finish_op(flash, rec, FFSV_OP_BLANK_CHECK, size,
                    FFSV_ERR_NOT_BLANK, 0);
        }
    }
    int result = inject ? injected_status(flash) : FFSV_OK;
    return finish_op(flash, rec, FFSV_OP_BLANK_CHECK, size, result, 0);
}

int ffsv_flash_program(struct ffsv_flash *flash, size_t offset,
        const void *buffer, size_t size, const char *call_site) {
    if (!flash || (!buffer && size)) {
        return FFSV_ERR_INVALID;
    }
    if (flash->staged.active) {
        return FFSV_ERR_STAGED_BUSY;
    }
    int err = check_range(flash, offset, size);
    if (!err) {
        err = check_alignment(offset, size, flash->cfg.program_granule);
    }

    bool inject = false;
    struct ffsv_op_record *rec = begin_op(flash, FFSV_OP_PROGRAM,
            offset, size, call_site, &inject);
    if (err) {
        return finish_op(flash, rec, FFSV_OP_PROGRAM, size, err, 0);
    }
    if (inject && flash->failure.phase == FFSV_FAIL_BEFORE) {
        return finish_op(flash, rec, FFSV_OP_PROGRAM, size,
                injected_status(flash), 0);
    }

    size_t commit = size;
    int result = FFSV_OK;
    if (inject && flash->failure.phase == FFSV_FAIL_MIDDLE) {
        commit = injected_partial(flash, size, flash->cfg.program_granule);
        result = injected_status(flash);
    } else if (inject && flash->failure.phase == FFSV_FAIL_AFTER) {
        result = injected_status(flash);
    }

    err = validate_program_transition(flash, offset, buffer, commit);
    if (err) {
        return finish_op(flash, rec, FFSV_OP_PROGRAM, size, err, 0);
    }
    if (commit > 0) {
        err = emu_program_range(flash, offset, buffer, commit);
        if (err) {
            return finish_op(flash, rec, FFSV_OP_PROGRAM, size, err, 0);
        }
    }
    return finish_op(flash, rec, FFSV_OP_PROGRAM, size, result, commit);
}

int ffsv_flash_erase(struct ffsv_flash *flash, size_t offset,
        size_t size, const char *call_site) {
    if (!flash) {
        return FFSV_ERR_INVALID;
    }
    int err = check_range(flash, offset, size);
    if (!err) {
        err = check_alignment(offset, size, flash->cfg.sector_size);
    }

    bool inject = false;
    struct ffsv_op_record *rec = begin_op(flash, FFSV_OP_ERASE,
            offset, size, call_site, &inject);
    if (err) {
        return finish_op(flash, rec, FFSV_OP_ERASE, size, err, 0);
    }
    if (inject && flash->failure.phase == FFSV_FAIL_BEFORE) {
        return finish_op(flash, rec, FFSV_OP_ERASE, size,
                injected_status(flash), 0);
    }

    size_t commit = size;
    int result = FFSV_OK;
    if (inject && flash->failure.phase == FFSV_FAIL_MIDDLE) {
        commit = injected_partial(flash, size, flash->cfg.sector_size);
        result = injected_status(flash);
    } else if (inject && flash->failure.phase == FFSV_FAIL_AFTER) {
        result = injected_status(flash);
    }

    if (commit == size) {
        for (size_t pos = offset; pos < offset + size;
                pos += flash->cfg.sector_size) {
            err = lfs_emubd_erase(&flash->lfs_cfg,
                    (lfs_block_t)(pos / flash->cfg.sector_size));
            if (err) {
                return finish_op(flash, rec, FFSV_OP_ERASE, size,
                        map_lfs_error(err), 0);
            }
        }
        refresh_image_cache(flash);
    } else if (commit > 0) {
        err = raw_erase_prefix(flash, offset, commit);
        if (err) {
            return finish_op(flash, rec, FFSV_OP_ERASE, size, err, 0);
        }
    }
    return finish_op(flash, rec, FFSV_OP_ERASE, size, result, commit);
}

int ffsv_flash_stage_program(struct ffsv_flash *flash, size_t offset,
        const void *buffer, size_t size, const char *call_site) {
    if (!flash || (!buffer && size)) {
        return FFSV_ERR_INVALID;
    }
    if (flash->staged.active) {
        return FFSV_ERR_STAGED_BUSY;
    }
    int err = check_range(flash, offset, size);
    if (!err) {
        err = check_alignment(offset, size, flash->cfg.program_granule);
    }

    bool inject = false;
    struct ffsv_op_record *rec = begin_op(flash, FFSV_OP_STAGE_PROGRAM,
            offset, size, call_site, &inject);
    if (err) {
        return finish_op(flash, rec, FFSV_OP_STAGE_PROGRAM, size, err, 0);
    }
    if (inject && flash->failure.phase == FFSV_FAIL_BEFORE) {
        return finish_op(flash, rec, FFSV_OP_STAGE_PROGRAM, size,
                injected_status(flash), 0);
    }

    err = validate_program_transition(flash, offset, buffer, size);
    if (err) {
        return finish_op(flash, rec, FFSV_OP_STAGE_PROGRAM, size, err, 0);
    }
    uint8_t *data = malloc(size);
    if (!data && size) {
        return finish_op(flash, rec, FFSV_OP_STAGE_PROGRAM, size,
                FFSV_ERR_NOMEM, 0);
    }
    memcpy(data, buffer, size);
    flash->staged = (struct staged_mutation){
        .active = true,
        .offset = offset,
        .size = size,
        .data = data,
    };
    int result = inject ? injected_status(flash) : FFSV_OK;
    return finish_op(flash, rec, FFSV_OP_STAGE_PROGRAM, size, result, 0);
}

int ffsv_flash_commit_staged(struct ffsv_flash *flash,
        const char *call_site) {
    if (!flash) {
        return FFSV_ERR_INVALID;
    }
    size_t offset = flash->staged.offset;
    size_t size = flash->staged.size;
    bool inject = false;
    struct ffsv_op_record *rec = begin_op(flash, FFSV_OP_COMMIT_STAGED,
            offset, size, call_site, &inject);
    if (!flash->staged.active) {
        return finish_op(flash, rec, FFSV_OP_COMMIT_STAGED, 0,
                FFSV_ERR_NO_STAGED_MUTATION, 0);
    }
    if (inject && flash->failure.phase == FFSV_FAIL_BEFORE) {
        return finish_op(flash, rec, FFSV_OP_COMMIT_STAGED, size,
                injected_status(flash), 0);
    }

    size_t commit = size;
    int result = FFSV_OK;
    if (inject && flash->failure.phase == FFSV_FAIL_MIDDLE) {
        commit = injected_partial(flash, size, flash->cfg.program_granule);
        result = injected_status(flash);
    } else if (inject && flash->failure.phase == FFSV_FAIL_AFTER) {
        result = injected_status(flash);
    }
    if (commit > 0) {
        int err = emu_program_range(flash, offset, flash->staged.data, commit);
        if (err != FFSV_OK) {
            return finish_op(flash, rec, FFSV_OP_COMMIT_STAGED, size, err, 0);
        }
    }
    free(flash->staged.data);
    flash->staged = (struct staged_mutation){0};
    return finish_op(flash, rec, FFSV_OP_COMMIT_STAGED, size, result, commit);
}

int ffsv_flash_drop_staged(struct ffsv_flash *flash,
        const char *call_site) {
    if (!flash) {
        return FFSV_ERR_INVALID;
    }
    bool inject = false;
    struct ffsv_op_record *rec = begin_op(flash, FFSV_OP_DROP_STAGED,
            flash->staged.offset, flash->staged.size, call_site, &inject);
    if (!flash->staged.active) {
        return finish_op(flash, rec, FFSV_OP_DROP_STAGED, 0,
                FFSV_ERR_NO_STAGED_MUTATION, 0);
    }
    if (inject && flash->failure.phase == FFSV_FAIL_BEFORE) {
        return finish_op(flash, rec, FFSV_OP_DROP_STAGED,
                flash->staged.size, injected_status(flash), 0);
    }
    size_t size = flash->staged.size;
    free(flash->staged.data);
    flash->staged = (struct staged_mutation){0};
    int result = inject ? injected_status(flash) : FFSV_OK;
    return finish_op(flash, rec, FFSV_OP_DROP_STAGED, size, result, 0);
}

static int corrupt_with_injection(struct ffsv_flash *flash, size_t offset,
        const uint8_t *buffer, size_t size, const char *call_site,
        bool xor_data) {
    int err = check_range(flash, offset, size);
    bool inject = false;
    struct ffsv_op_record *rec = begin_op(flash, FFSV_OP_CORRUPT,
            offset, size, call_site, &inject);
    if (err) {
        return finish_op(flash, rec, FFSV_OP_CORRUPT, size, err, 0);
    }
    if (inject && flash->failure.phase == FFSV_FAIL_BEFORE) {
        return finish_op(flash, rec, FFSV_OP_CORRUPT, size,
                injected_status(flash), 0);
    }

    size_t commit = size;
    int result = FFSV_OK;
    if (inject && flash->failure.phase == FFSV_FAIL_MIDDLE) {
        commit = injected_partial(flash, size, 1);
        result = injected_status(flash);
    } else if (inject && flash->failure.phase == FFSV_FAIL_AFTER) {
        result = injected_status(flash);
    }
    if (commit > 0) {
        err = raw_corrupt(flash, offset, buffer, commit, xor_data);
        if (err) {
            return finish_op(flash, rec, FFSV_OP_CORRUPT, size, err, 0);
        }
    }
    return finish_op(flash, rec, FFSV_OP_CORRUPT, size, result, commit);
}

int ffsv_flash_corrupt(struct ffsv_flash *flash, size_t offset,
        const void *buffer, size_t size, const char *call_site) {
    if (!flash || (!buffer && size)) {
        return FFSV_ERR_INVALID;
    }
    return corrupt_with_injection(flash, offset, buffer, size, call_site,
            false);
}

int ffsv_flash_xor(struct ffsv_flash *flash, size_t offset,
        const uint8_t *mask, size_t size, const char *call_site) {
    if (!flash || (!mask && size)) {
        return FFSV_ERR_INVALID;
    }
    return corrupt_with_injection(flash, offset, mask, size, call_site,
            true);
}

int ffsv_flash_snapshot_create(const struct ffsv_flash *flash,
        struct ffsv_flash_snapshot *snapshot) {
    if (!flash || !snapshot) {
        return FFSV_ERR_INVALID;
    }
    *snapshot = (struct ffsv_flash_snapshot){0};
    snapshot->image = malloc(flash->cfg.total_size);
    snapshot->wear = calloc(flash->sector_count, sizeof(*snapshot->wear));
    if (!snapshot->image || !snapshot->wear) {
        ffsv_flash_snapshot_destroy(snapshot);
        return FFSV_ERR_NOMEM;
    }

    const uint8_t *image = ffsv_flash_image((struct ffsv_flash *)flash);
    memcpy(snapshot->image, image, flash->cfg.total_size);
    for (size_t i = 0; i < flash->sector_count; i++) {
        snapshot->wear[i] = ffsv_flash_sector_wear(flash, i);
    }
    snapshot->cfg = flash->cfg;
    snapshot->size = flash->cfg.total_size;
    snapshot->sector_count = flash->sector_count;
    snapshot->next_sequence = flash->next_sequence;
    snapshot->time_ns = flash->time_ns;
    return FFSV_OK;
}

void ffsv_flash_snapshot_destroy(struct ffsv_flash_snapshot *snapshot) {
    if (!snapshot) {
        return;
    }
    free(snapshot->image);
    free(snapshot->wear);
    *snapshot = (struct ffsv_flash_snapshot){0};
}

int ffsv_flash_reopen_from_snapshot(struct ffsv_flash **out,
        const struct ffsv_flash_snapshot *snapshot) {
    if (!out || !snapshot || !snapshot->image || !snapshot->wear ||
            checked_config(&snapshot->cfg) != FFSV_OK ||
            snapshot->size != snapshot->cfg.total_size) {
        return FFSV_ERR_INVALID;
    }
    if (snapshot->sector_count !=
            snapshot->cfg.total_size / snapshot->cfg.sector_size) {
        return FFSV_ERR_INVALID;
    }

    struct ffsv_flash *flash = NULL;
    int err = ffsv_flash_create(&flash, &snapshot->cfg);
    if (err) {
        return err;
    }
    err = raw_load_image(flash, snapshot->image, snapshot->size,
            snapshot->wear);
    if (err) {
        ffsv_flash_destroy(flash);
        return err;
    }
    flash->next_sequence = snapshot->next_sequence;
    flash->time_ns = snapshot->time_ns;
    *out = flash;
    return FFSV_OK;
}

void ffsv_flash_clear_failure(struct ffsv_flash *flash) {
    if (flash) {
        flash->failure = (struct ffsv_failure_injection){0};
    }
}

void ffsv_flash_set_failure(struct ffsv_flash *flash,
        const struct ffsv_failure_injection *failure) {
    if (flash && failure) {
        flash->failure = *failure;
    }
}

const uint8_t *ffsv_flash_image(struct ffsv_flash *flash) {
    if (!flash) {
        return NULL;
    }
    refresh_image_cache(flash);
    return flash->image_cache;
}

size_t ffsv_flash_size(const struct ffsv_flash *flash) {
    return flash ? flash->cfg.total_size : 0;
}

uint8_t ffsv_flash_image_byte(struct ffsv_flash *flash, size_t offset) {
    const uint8_t *image = ffsv_flash_image(flash);
    if (!image || offset >= flash->cfg.total_size) {
        return 0;
    }
    return image[offset];
}

bool ffsv_flash_image_span_is_erased(struct ffsv_flash *flash,
        size_t offset, size_t size) {
    if (!flash || check_range(flash, offset, size) != FFSV_OK) {
        return false;
    }
    const uint8_t *image = ffsv_flash_image(flash);
    for (size_t i = 0; i < size; i++) {
        if (image[offset + i] != flash->cfg.erased_value) {
            return false;
        }
    }
    return true;
}

uint64_t ffsv_flash_time_ns(const struct ffsv_flash *flash) {
    return flash ? flash->time_ns : 0;
}

uint64_t ffsv_flash_next_sequence(const struct ffsv_flash *flash) {
    return flash ? flash->next_sequence : 0;
}

uint32_t ffsv_flash_sector_wear(const struct ffsv_flash *flash,
        size_t sector) {
    if (!flash || sector >= flash->sector_count) {
        return 0;
    }
    return (uint32_t)lfs_emubd_wear(&flash->lfs_cfg, (lfs_block_t)sector);
}

const struct ffsv_op_record *ffsv_flash_log(
        const struct ffsv_flash *flash, size_t *count) {
    if (count) {
        *count = flash ? flash->log_count : 0;
    }
    return flash ? flash->log : NULL;
}

const struct ffsv_op_counts *ffsv_flash_counts(
        const struct ffsv_flash *flash) {
    return flash ? flash->counts : NULL;
}

const struct ffsv_sector_counts *ffsv_flash_sector_counts(
        const struct ffsv_flash *flash, size_t *count) {
    if (count) {
        *count = flash ? flash->sector_count : 0;
    }
    return flash ? flash->sector_counts : NULL;
}

uint64_t ffsv_flash_count_matching(const struct ffsv_flash *flash,
        enum ffsv_op_type type, size_t size, size_t sector,
        const char *call_site) {
    if (!flash) {
        return 0;
    }
    uint64_t matches = 0;
    for (size_t i = 0; i < flash->log_count; i++) {
        const struct ffsv_op_record *r = &flash->log[i];
        if (type < FFSV_OP_COUNT && r->type != type) {
            continue;
        }
        if (size != SIZE_MAX && r->size != size) {
            continue;
        }
        if (sector != SIZE_MAX && r->sector != sector) {
            continue;
        }
        if (call_site && (!r->call_site ||
                strcmp(r->call_site, call_site) != 0)) {
            continue;
        }
        matches += 1;
    }
    return matches;
}

int ffsv_flash_dump_image(struct ffsv_flash *flash, const char *path) {
    if (!flash || !path) {
        return FFSV_ERR_INVALID;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return FFSV_ERR_IO;
    }
    const uint8_t *image = ffsv_flash_image(flash);
    size_t n = fwrite(image, 1, flash->cfg.total_size, f);
    int close_err = fclose(f);
    if (n != flash->cfg.total_size || close_err != 0) {
        return FFSV_ERR_IO;
    }
    return FFSV_OK;
}

int ffsv_flash_load_image(struct ffsv_flash *flash,
        const void *image, size_t size) {
    if (!flash || (!image && size)) {
        return FFSV_ERR_INVALID;
    }
    return raw_load_image(flash, image, size, NULL);
}

int ffsv_flash_load_image_file(struct ffsv_flash *flash, const char *path) {
    if (!flash || !path) {
        return FFSV_ERR_INVALID;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return FFSV_ERR_IO;
    }
    uint8_t *image = malloc(flash->cfg.total_size);
    if (!image) {
        fclose(f);
        return FFSV_ERR_NOMEM;
    }
    size_t n = fread(image, 1, flash->cfg.total_size, f);
    int extra = fgetc(f);
    int close_err = fclose(f);
    int err = FFSV_OK;
    if (n != flash->cfg.total_size || extra != EOF || close_err != 0) {
        err = FFSV_ERR_IO;
    } else {
        err = ffsv_flash_load_image(flash, image, flash->cfg.total_size);
    }
    free(image);
    return err;
}

int ffsv_flash_dump_log(const struct ffsv_flash *flash, FILE *out) {
    if (!flash || !out) {
        return FFSV_ERR_INVALID;
    }
    fprintf(out, "seq,type,offset,size,sector,time_before_ns,time_after_ns,"
            "result,injected,phase,committed_bytes,call_site\n");
    for (size_t i = 0; i < flash->log_count; i++) {
        const struct ffsv_op_record *r = &flash->log[i];
        fprintf(out, "%" PRIu64 ",%s,%zu,%zu,%zu,%" PRIu64 ",%" PRIu64
                ",%s,%d,%d,%zu,%s\n",
                r->sequence, ffsv_op_name(r->type), r->offset, r->size,
                r->sector, r->time_before_ns, r->time_after_ns,
                ffsv_status_name(r->result), r->injected,
                r->injected_phase, r->committed_bytes,
                r->call_site ? r->call_site : "");
    }
    return ferror(out) ? FFSV_ERR_IO : FFSV_OK;
}

int ffsv_flash_dump_timeline(const struct ffsv_flash *flash, FILE *out) {
    if (!flash || !out) {
        return FFSV_ERR_INVALID;
    }
    for (size_t i = 0; i < flash->log_count; i++) {
        const struct ffsv_op_record *r = &flash->log[i];
        fprintf(out, "[%" PRIu64 "..%" PRIu64 "] #%" PRIu64
                " %s off=%zu size=%zu result=%s committed=%zu site=%s\n",
                r->time_before_ns, r->time_after_ns, r->sequence,
                ffsv_op_name(r->type), r->offset, r->size,
                ffsv_status_name(r->result), r->committed_bytes,
                r->call_site ? r->call_site : "");
    }
    return ferror(out) ? FFSV_ERR_IO : FFSV_OK;
}

const char *ffsv_op_name(enum ffsv_op_type type) {
    switch (type) {
    case FFSV_OP_READ:
        return "read";
    case FFSV_OP_PROGRAM:
        return "program";
    case FFSV_OP_ERASE:
        return "erase";
    case FFSV_OP_BLANK_CHECK:
        return "blank_check";
    case FFSV_OP_STAGE_PROGRAM:
        return "stage_program";
    case FFSV_OP_COMMIT_STAGED:
        return "commit_staged";
    case FFSV_OP_DROP_STAGED:
        return "drop_staged";
    case FFSV_OP_CORRUPT:
        return "corrupt";
    default:
        return "unknown";
    }
}

const char *ffsv_status_name(int status) {
    switch (status) {
    case FFSV_OK:
        return "ok";
    case FFSV_ERR_INVALID:
        return "invalid";
    case FFSV_ERR_NOMEM:
        return "nomem";
    case FFSV_ERR_RANGE:
        return "range";
    case FFSV_ERR_ALIGNMENT:
        return "alignment";
    case FFSV_ERR_NOT_BLANK:
        return "not_blank";
    case FFSV_ERR_PROGRAM_TRANSITION:
        return "program_transition";
    case FFSV_ERR_STAGED_BUSY:
        return "staged_busy";
    case FFSV_ERR_NO_STAGED_MUTATION:
        return "no_staged_mutation";
    case FFSV_ERR_INJECTED:
        return "injected";
    case FFSV_ERR_IO:
        return "io";
    default:
        return "unknown";
    }
}
