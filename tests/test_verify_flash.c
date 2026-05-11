#include "fastffs/verify_flash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_OK(expr) do { \
    int rc__ = (expr); \
    if (rc__ != FFSV_OK) { \
        fprintf(stderr, "%s:%d: expected ok, got %s\n", \
                __FILE__, __LINE__, ffsv_status_name(rc__)); \
        return 1; \
    } \
} while (0)

#define ASSERT_EQ_INT(exp, got) do { \
    int exp__ = (exp); \
    int got__ = (got); \
    if (exp__ != got__) { \
        fprintf(stderr, "%s:%d: expected %d, got %d\n", \
                __FILE__, __LINE__, exp__, got__); \
        return 1; \
    } \
} while (0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: assertion failed: %s\n", \
                __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static struct ffsv_flash_config test_cfg(void) {
    return (struct ffsv_flash_config){
        .total_size = 4096 * 4,
        .sector_size = 4096,
        .program_granule = 4,
        .read_granule = 1,
        .erased_value = 0xff,
        .erase_cycles = 100000,
        .timing = {
            .read_fixed_ns = 10,
            .read_per_byte_ns = 1,
            .program_fixed_ns = 100,
            .program_per_byte_ns = 2,
            .erase_fixed_ns = 1000,
            .erase_per_byte_ns = 3,
            .blank_check_fixed_ns = 20,
            .blank_check_per_byte_ns = 1,
        },
        .max_log_entries = 128,
    };
}

static int test_nor_semantics_and_wear(void) {
    struct ffsv_flash *flash = NULL;
    struct ffsv_flash_config cfg = test_cfg();
    const char *site = "nor";
    ASSERT_OK(ffsv_flash_create(&flash, &cfg));

    uint8_t first[4] = {0xf0, 0x0f, 0xaa, 0x55};
    uint8_t second[4] = {0xe0, 0x0e, 0x88, 0x11};
    uint8_t illegal[4] = {0xff, 0xff, 0xff, 0xff};
    uint8_t out[4] = {0};

    ASSERT_OK(ffsv_flash_blank_check(flash, 0, sizeof(first), site));
    ASSERT_OK(ffsv_flash_program(flash, 0, first, sizeof(first), site));
    ASSERT_OK(ffsv_flash_program(flash, 0, second, sizeof(second), site));
    ASSERT_OK(ffsv_flash_read(flash, 0, out, sizeof(out), site));
    ASSERT_TRUE(memcmp(out, second, sizeof(second)) == 0);
    ASSERT_EQ_INT(FFSV_ERR_PROGRAM_TRANSITION,
            ffsv_flash_program(flash, 0, illegal, sizeof(illegal), site));

    ASSERT_OK(ffsv_flash_erase(flash, 0, 4096, site));
    ASSERT_OK(ffsv_flash_blank_check(flash, 0, 4096, site));
    ASSERT_TRUE(ffsv_flash_sector_wear(flash, 0) == 1);

    const struct ffsv_op_counts *counts = ffsv_flash_counts(flash);
    ASSERT_TRUE(counts[FFSV_OP_PROGRAM].calls == 3);
    ASSERT_TRUE(ffsv_flash_time_ns(flash) > 0);

    ffsv_flash_destroy(flash);
    return 0;
}

static int test_failure_injection_and_staging(void) {
    struct ffsv_flash *flash = NULL;
    struct ffsv_flash_config cfg = test_cfg();
    ASSERT_OK(ffsv_flash_create(&flash, &cfg));

    uint8_t data[8] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    ffsv_flash_set_failure(flash, &(struct ffsv_failure_injection){
        .enabled = true,
        .sequence = ffsv_flash_next_sequence(flash),
        .op_mask = UINT32_C(1) << FFSV_OP_PROGRAM,
        .phase = FFSV_FAIL_MIDDLE,
        .status = FFSV_ERR_INJECTED,
        .partial_bytes = 4,
    });
    ASSERT_EQ_INT(FFSV_ERR_INJECTED,
            ffsv_flash_program(flash, 0, data, sizeof(data), FFSV_CALLSITE));
    ASSERT_TRUE(memcmp(ffsv_flash_image(flash), data, 4) == 0);
    ASSERT_TRUE(ffsv_flash_image(flash)[4] == 0xff);

    ASSERT_OK(ffsv_flash_erase(flash, 0, 4096, FFSV_CALLSITE));
    ffsv_flash_clear_failure(flash);
    ASSERT_OK(ffsv_flash_stage_program(flash, 32, data, sizeof(data),
                FFSV_CALLSITE));
    ASSERT_TRUE(ffsv_flash_image(flash)[32] == 0xff);
    ASSERT_OK(ffsv_flash_commit_staged(flash, FFSV_CALLSITE));
    ASSERT_TRUE(memcmp(ffsv_flash_image(flash) + 32, data, sizeof(data)) == 0);

    size_t count = 0;
    const struct ffsv_op_record *log = ffsv_flash_log(flash, &count);
    ASSERT_TRUE(count >= 4);
    ASSERT_TRUE(log[0].injected && log[0].committed_bytes == 4);

    ffsv_flash_destroy(flash);
    return 0;
}

static int test_corruption_and_dumps(void) {
    struct ffsv_flash *flash = NULL;
    struct ffsv_flash_config cfg = test_cfg();
    ASSERT_OK(ffsv_flash_create(&flash, &cfg));

    uint8_t replacement[4] = {0x12, 0x34, 0x56, 0x78};
    uint8_t mask[4] = {0xff, 0x00, 0xff, 0x00};
    ASSERT_OK(ffsv_flash_corrupt(flash, 64, replacement,
                sizeof(replacement), FFSV_CALLSITE));
    ASSERT_OK(ffsv_flash_xor(flash, 64, mask, sizeof(mask), FFSV_CALLSITE));
    ASSERT_TRUE(ffsv_flash_image(flash)[64] == (uint8_t)(0x12 ^ 0xff));
    ASSERT_TRUE(ffsv_flash_image(flash)[65] == 0x34);

    FILE *tmp = tmpfile();
    ASSERT_TRUE(tmp != NULL);
    ASSERT_OK(ffsv_flash_dump_log(flash, tmp));
    ASSERT_OK(ffsv_flash_dump_timeline(flash, tmp));
    fclose(tmp);
    ASSERT_OK(ffsv_flash_dump_image(flash,
                "/tmp/fastffs_verify_flash_test.img"));

    ffsv_flash_destroy(flash);
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_nor_semantics_and_wear();
    failures += test_failure_injection_and_staging();
    failures += test_corruption_and_dumps();
    if (failures) {
        fprintf(stderr, "%d verify_flash tests failed\n", failures);
        return 1;
    }
    printf("verify_flash tests passed\n");
    return 0;
}
