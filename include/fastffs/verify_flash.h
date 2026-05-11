#ifndef FASTFFS_VERIFY_FLASH_H
#define FASTFFS_VERIFY_FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ffsv_status {
    FFSV_OK = 0,
    FFSV_ERR_INVALID = -1,
    FFSV_ERR_NOMEM = -2,
    FFSV_ERR_RANGE = -3,
    FFSV_ERR_ALIGNMENT = -4,
    FFSV_ERR_NOT_BLANK = -5,
    FFSV_ERR_PROGRAM_TRANSITION = -6,
    FFSV_ERR_STAGED_BUSY = -7,
    FFSV_ERR_NO_STAGED_MUTATION = -8,
    FFSV_ERR_INJECTED = -9,
    FFSV_ERR_IO = -10,
};

enum ffsv_op_type {
    FFSV_OP_READ = 0,
    FFSV_OP_PROGRAM = 1,
    FFSV_OP_ERASE = 2,
    FFSV_OP_BLANK_CHECK = 3,
    FFSV_OP_STAGE_PROGRAM = 4,
    FFSV_OP_COMMIT_STAGED = 5,
    FFSV_OP_DROP_STAGED = 6,
    FFSV_OP_CORRUPT = 7,
    FFSV_OP_COUNT = 8,
};

enum ffsv_failure_phase {
    FFSV_FAIL_BEFORE = 0,
    FFSV_FAIL_MIDDLE = 1,
    FFSV_FAIL_AFTER = 2,
};

struct ffsv_timing {
    uint64_t read_fixed_ns;
    uint64_t read_per_byte_ns;
    uint64_t program_fixed_ns;
    uint64_t program_per_byte_ns;
    uint64_t erase_fixed_ns;
    uint64_t erase_per_byte_ns;
    uint64_t blank_check_fixed_ns;
    uint64_t blank_check_per_byte_ns;
};

struct ffsv_flash_config {
    size_t total_size;
    size_t sector_size;
    size_t program_granule;
    size_t read_granule;
    uint8_t erased_value;
    uint32_t erase_cycles;
    struct ffsv_timing timing;
    size_t max_log_entries;
};

struct ffsv_failure_injection {
    bool enabled;
    uint64_t sequence;
    uint32_t op_mask;
    enum ffsv_failure_phase phase;
    int status;
    size_t partial_bytes;
};

struct ffsv_op_record {
    uint64_t sequence;
    enum ffsv_op_type type;
    size_t offset;
    size_t size;
    size_t sector;
    const char *call_site;
    uint64_t time_before_ns;
    uint64_t time_after_ns;
    int result;
    bool injected;
    enum ffsv_failure_phase injected_phase;
    size_t committed_bytes;
};

struct ffsv_op_counts {
    uint64_t calls;
    uint64_t bytes;
};

struct ffsv_sector_counts {
    uint64_t read_calls;
    uint64_t read_bytes;
    uint64_t programmed_bytes;
    uint64_t erased_bytes;
    uint64_t blank_checked_bytes;
};

struct ffsv_flash;

#define FFSV_STRINGIFY_(x) #x
#define FFSV_STRINGIFY(x) FFSV_STRINGIFY_(x)
#define FFSV_CALLSITE __FILE__ ":" FFSV_STRINGIFY(__LINE__)

int ffsv_flash_create(struct ffsv_flash **out,
        const struct ffsv_flash_config *cfg);
void ffsv_flash_destroy(struct ffsv_flash *flash);

int ffsv_flash_read(struct ffsv_flash *flash, size_t offset,
        void *buffer, size_t size, const char *call_site);
int ffsv_flash_program(struct ffsv_flash *flash, size_t offset,
        const void *buffer, size_t size, const char *call_site);
int ffsv_flash_erase(struct ffsv_flash *flash, size_t offset,
        size_t size, const char *call_site);
int ffsv_flash_blank_check(struct ffsv_flash *flash, size_t offset,
        size_t size, const char *call_site);

int ffsv_flash_stage_program(struct ffsv_flash *flash, size_t offset,
        const void *buffer, size_t size, const char *call_site);
int ffsv_flash_commit_staged(struct ffsv_flash *flash,
        const char *call_site);
int ffsv_flash_drop_staged(struct ffsv_flash *flash,
        const char *call_site);

int ffsv_flash_corrupt(struct ffsv_flash *flash, size_t offset,
        const void *buffer, size_t size, const char *call_site);
int ffsv_flash_xor(struct ffsv_flash *flash, size_t offset,
        const uint8_t *mask, size_t size, const char *call_site);

void ffsv_flash_clear_failure(struct ffsv_flash *flash);
void ffsv_flash_set_failure(struct ffsv_flash *flash,
        const struct ffsv_failure_injection *failure);

const uint8_t *ffsv_flash_image(struct ffsv_flash *flash);
size_t ffsv_flash_size(const struct ffsv_flash *flash);
uint64_t ffsv_flash_time_ns(const struct ffsv_flash *flash);
uint64_t ffsv_flash_next_sequence(const struct ffsv_flash *flash);
uint32_t ffsv_flash_sector_wear(const struct ffsv_flash *flash,
        size_t sector);

const struct ffsv_op_record *ffsv_flash_log(
        const struct ffsv_flash *flash, size_t *count);
const struct ffsv_op_counts *ffsv_flash_counts(
        const struct ffsv_flash *flash);
const struct ffsv_sector_counts *ffsv_flash_sector_counts(
        const struct ffsv_flash *flash, size_t *count);
uint64_t ffsv_flash_count_matching(const struct ffsv_flash *flash,
        enum ffsv_op_type type, size_t size, size_t sector,
        const char *call_site);

int ffsv_flash_dump_image(struct ffsv_flash *flash, const char *path);
int ffsv_flash_dump_log(const struct ffsv_flash *flash, FILE *out);
int ffsv_flash_dump_timeline(const struct ffsv_flash *flash, FILE *out);

const char *ffsv_op_name(enum ffsv_op_type type);
const char *ffsv_status_name(int status);

#ifdef __cplusplus
}
#endif

#endif
