#include "fastffs/verify_flash.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
        .max_log_entries = 256,
    };
}

static void inject_next(struct ffsv_flash *flash, enum ffsv_op_type op,
        enum ffsv_failure_phase phase, size_t partial_bytes) {
    ffsv_flash_set_failure(flash, &(struct ffsv_failure_injection){
        .enabled = true,
        .sequence = ffsv_flash_next_sequence(flash),
        .op_mask = UINT32_C(1) << op,
        .phase = phase,
        .status = FFSV_ERR_INJECTED,
        .partial_bytes = partial_bytes,
    });
}

static int new_flash(struct ffsv_flash **flash) {
    struct ffsv_flash_config cfg = test_cfg();
    return ffsv_flash_create(flash, &cfg);
}

static int test_nor_semantics_and_wear(void) {
    struct ffsv_flash *flash = NULL;
    const char *site = "nor";
    ASSERT_OK(new_flash(&flash));

    uint8_t first[4] = {0xf0, 0x0f, 0xaa, 0x55};
    uint8_t second[4] = {0xe0, 0x0e, 0x88, 0x11};
    uint8_t illegal[4] = {0xff, 0xff, 0xff, 0xff};
    uint8_t out[4] = {0};

    ASSERT_OK(ffsv_flash_blank_check(flash, 0, sizeof(first), site));
    ASSERT_OK(ffsv_flash_program(flash, 0, first, sizeof(first), site));
    ASSERT_OK(ffsv_flash_program(flash, 0, second, sizeof(second), site));
    ASSERT_OK(ffsv_flash_read(flash, 0, out, sizeof(out), site));
    ASSERT_TRUE(memcmp(out, second, sizeof(second)) == 0);
    ASSERT_OK(ffsv_flash_program(flash, 0, illegal, sizeof(illegal), site));
    ASSERT_OK(ffsv_flash_read(flash, 0, out, sizeof(out), site));
    ASSERT_TRUE(memcmp(out, second, sizeof(second)) == 0);

    ASSERT_OK(ffsv_flash_erase(flash, 0, 4096, site));
    ASSERT_OK(ffsv_flash_blank_check(flash, 0, 4096, site));
    ASSERT_TRUE(ffsv_flash_sector_wear(flash, 0) == 1);
    ASSERT_TRUE(ffsv_flash_time_ns(flash) > 0);

    ffsv_flash_destroy(flash);
    return 0;
}

static int test_failure_matrix_program(void) {
    const uint8_t data[8] = {0x00, 0x11, 0x22, 0x33,
        0x44, 0x55, 0x66, 0x77};
    const enum ffsv_failure_phase phases[] = {
        FFSV_FAIL_BEFORE, FFSV_FAIL_MIDDLE, FFSV_FAIL_AFTER,
    };
    const size_t committed[] = {0, 4, 8};

    for (size_t i = 0; i < 3; i++) {
        struct ffsv_flash *flash = NULL;
        ASSERT_OK(new_flash(&flash));
        inject_next(flash, FFSV_OP_PROGRAM, phases[i], 4);
        ASSERT_EQ_INT(FFSV_ERR_INJECTED,
                ffsv_flash_program(flash, 0, data, sizeof(data),
                    FFSV_CALLSITE));
        ASSERT_TRUE(memcmp(ffsv_flash_image(flash), data, committed[i]) == 0);
        ASSERT_TRUE(ffsv_flash_image_span_is_erased(flash, committed[i],
                    sizeof(data) - committed[i]));
        size_t count = 0;
        const struct ffsv_op_record *log = ffsv_flash_log(flash, &count);
        ASSERT_TRUE(count == 1);
        ASSERT_TRUE(log[0].injected && log[0].committed_bytes == committed[i]);
        ffsv_flash_destroy(flash);
    }
    return 0;
}

static int test_failure_matrix_erase_commit_and_corrupt(void) {
    const uint8_t zeros[8192] = {0};
    const uint8_t data[8] = {0x10, 0x20, 0x30, 0x40,
        0x50, 0x60, 0x70, 0x80};
    const enum ffsv_failure_phase phases[] = {
        FFSV_FAIL_BEFORE, FFSV_FAIL_MIDDLE, FFSV_FAIL_AFTER,
    };

    for (size_t i = 0; i < 3; i++) {
        struct ffsv_flash *flash = NULL;
        ASSERT_OK(new_flash(&flash));
        ASSERT_OK(ffsv_flash_program(flash, 0, zeros, sizeof(zeros),
                    FFSV_CALLSITE));
        inject_next(flash, FFSV_OP_ERASE, phases[i], 4096);
        ASSERT_EQ_INT(FFSV_ERR_INJECTED,
                ffsv_flash_erase(flash, 0, sizeof(zeros), FFSV_CALLSITE));
        ASSERT_TRUE(ffsv_flash_image_span_is_erased(flash, 0,
                    i == 0 ? 0 : (i == 1 ? 4096 : 8192)));
        if (i < 2) {
            ASSERT_TRUE(ffsv_flash_image_byte(flash,
                        i == 0 ? 0 : 4096) == 0x00);
        }
        ffsv_flash_destroy(flash);
    }

    for (size_t i = 0; i < 3; i++) {
        struct ffsv_flash *flash = NULL;
        ASSERT_OK(new_flash(&flash));
        ASSERT_OK(ffsv_flash_stage_program(flash, 32, data, sizeof(data),
                    FFSV_CALLSITE));
        inject_next(flash, FFSV_OP_COMMIT_STAGED, phases[i], 4);
        ASSERT_EQ_INT(FFSV_ERR_INJECTED,
                ffsv_flash_commit_staged(flash, FFSV_CALLSITE));
        size_t n = i == 0 ? 0 : (i == 1 ? 4 : 8);
        ASSERT_TRUE(memcmp(ffsv_flash_image(flash) + 32, data, n) == 0);
        ASSERT_TRUE(ffsv_flash_image_span_is_erased(flash, 32 + n,
                    sizeof(data) - n));
        if (i == 0) {
            ASSERT_OK(ffsv_flash_drop_staged(flash, FFSV_CALLSITE));
        }
        ffsv_flash_destroy(flash);
    }

    for (size_t i = 0; i < 3; i++) {
        struct ffsv_flash *flash = NULL;
        ASSERT_OK(new_flash(&flash));
        inject_next(flash, FFSV_OP_CORRUPT, phases[i], 3);
        ASSERT_EQ_INT(FFSV_ERR_INJECTED,
                ffsv_flash_corrupt(flash, 64, data, sizeof(data),
                    FFSV_CALLSITE));
        size_t n = i == 0 ? 0 : (i == 1 ? 3 : 8);
        ASSERT_TRUE(memcmp(ffsv_flash_image(flash) + 64, data, n) == 0);
        ASSERT_TRUE(ffsv_flash_image_span_is_erased(flash, 64 + n,
                    sizeof(data) - n));
        ffsv_flash_destroy(flash);
    }
    return 0;
}

static int test_drop_staged_before_after(void) {
    const uint8_t data[4] = {0, 1, 2, 3};
    struct ffsv_flash *flash = NULL;
    ASSERT_OK(new_flash(&flash));

    ASSERT_OK(ffsv_flash_stage_program(flash, 0, data, sizeof(data),
                FFSV_CALLSITE));
    inject_next(flash, FFSV_OP_DROP_STAGED, FFSV_FAIL_BEFORE, 0);
    ASSERT_EQ_INT(FFSV_ERR_INJECTED,
            ffsv_flash_drop_staged(flash, FFSV_CALLSITE));
    ASSERT_OK(ffsv_flash_commit_staged(flash, FFSV_CALLSITE));
    ASSERT_TRUE(memcmp(ffsv_flash_image(flash), data, sizeof(data)) == 0);

    ASSERT_OK(ffsv_flash_erase(flash, 0, 4096, FFSV_CALLSITE));
    ASSERT_OK(ffsv_flash_stage_program(flash, 0, data, sizeof(data),
                FFSV_CALLSITE));
    inject_next(flash, FFSV_OP_DROP_STAGED, FFSV_FAIL_AFTER, 0);
    ASSERT_EQ_INT(FFSV_ERR_INJECTED,
            ffsv_flash_drop_staged(flash, FFSV_CALLSITE));
    ASSERT_EQ_INT(FFSV_ERR_NO_STAGED_MUTATION,
            ffsv_flash_commit_staged(flash, FFSV_CALLSITE));

    ffsv_flash_destroy(flash);
    return 0;
}

static int test_snapshot_reopen_and_image_load(void) {
    struct ffsv_flash *flash = NULL;
    struct ffsv_flash *reopened = NULL;
    struct ffsv_flash *loaded = NULL;
    struct ffsv_flash_snapshot snapshot = {0};
    const uint8_t data[8] = {0xde, 0xad, 0xbe, 0xef,
        0xaa, 0x55, 0x33, 0xcc};
    char path[128];
    snprintf(path, sizeof(path), "/tmp/fastffs_verify_flash_test_%ld.img",
            (long)getpid());
    ASSERT_OK(new_flash(&flash));
    ASSERT_OK(ffsv_flash_program(flash, 128, data, sizeof(data),
                FFSV_CALLSITE));
    ASSERT_OK(ffsv_flash_erase(flash, 4096, 4096, FFSV_CALLSITE));
    ASSERT_OK(ffsv_flash_snapshot_create(flash, &snapshot));

    ASSERT_OK(ffsv_flash_reopen_from_snapshot(&reopened, &snapshot));
    ASSERT_TRUE(memcmp(ffsv_flash_image(reopened), ffsv_flash_image(flash),
                ffsv_flash_size(flash)) == 0);
    ASSERT_TRUE(ffsv_flash_sector_wear(reopened, 1) == 1);
    ASSERT_TRUE(ffsv_flash_time_ns(reopened) == ffsv_flash_time_ns(flash));

    struct ffsv_flash_snapshot malformed = snapshot;
    malformed.cfg.sector_size = 0;
    ASSERT_EQ_INT(FFSV_ERR_INVALID,
            ffsv_flash_reopen_from_snapshot(&loaded, &malformed));
    ASSERT_TRUE(loaded == NULL);

    ASSERT_OK(new_flash(&loaded));
    ASSERT_OK(ffsv_flash_load_image(loaded, ffsv_flash_image(flash),
                ffsv_flash_size(flash)));
    ASSERT_TRUE(ffsv_flash_image_byte(loaded, 128) == 0xde);
    ASSERT_TRUE(!ffsv_flash_image_span_is_erased(loaded, 128,
                sizeof(data)));
    ASSERT_OK(ffsv_flash_dump_image(loaded, path));
    ASSERT_OK(ffsv_flash_erase(loaded, 0, 4096, FFSV_CALLSITE));
    ASSERT_OK(ffsv_flash_load_image_file(loaded, path));
    remove(path);
    ASSERT_TRUE(memcmp(ffsv_flash_image(loaded), ffsv_flash_image(flash),
                ffsv_flash_size(flash)) == 0);

    ffsv_flash_snapshot_destroy(&snapshot);
    ffsv_flash_destroy(loaded);
    ffsv_flash_destroy(reopened);
    ffsv_flash_destroy(flash);
    return 0;
}

static int test_cross_sector_mutations_and_counts(void) {
    struct ffsv_flash *flash = NULL;
    const uint8_t data[8] = {0x0f, 0x1e, 0x2d, 0x3c,
        0x4b, 0x5a, 0x69, 0x78};
    const uint8_t replacement[8] = {0xa0, 0xa1, 0xa2, 0xa3,
        0xa4, 0xa5, 0xa6, 0xa7};
    ASSERT_OK(new_flash(&flash));

    ASSERT_OK(ffsv_flash_program(flash, 4092, data, sizeof(data), "cross"));
    ASSERT_TRUE(memcmp(ffsv_flash_image(flash) + 4092, data,
                sizeof(data)) == 0);
    ASSERT_OK(ffsv_flash_corrupt(flash, 4092, replacement,
                sizeof(replacement), "cross"));
    ASSERT_TRUE(memcmp(ffsv_flash_image(flash) + 4092, replacement,
                sizeof(replacement)) == 0);

    size_t sector_count = 0;
    const struct ffsv_sector_counts *sectors =
        ffsv_flash_sector_counts(flash, &sector_count);
    ASSERT_TRUE(sector_count == 4);
    ASSERT_TRUE(sectors[0].programmed_bytes == 4);
    ASSERT_TRUE(sectors[1].programmed_bytes == 4);
    ASSERT_TRUE(ffsv_flash_count_matching(flash, FFSV_OP_PROGRAM, 8, 0,
                "cross") == 1);

    ffsv_flash_destroy(flash);
    return 0;
}

static int test_log_filter_validation_and_dumps(void) {
    struct ffsv_flash *flash = NULL;
    const uint8_t data[4] = {0x12, 0x34, 0x56, 0x78};
    char buf[512] = {0};
    ASSERT_OK(new_flash(&flash));

    ASSERT_OK(ffsv_flash_program(flash, 0, data, sizeof(data), "alpha"));
    ASSERT_OK(ffsv_flash_read(flash, 0, buf, sizeof(data), "beta"));
    ASSERT_TRUE(ffsv_flash_count_matching(flash, FFSV_OP_PROGRAM, 4,
                0, "alpha") == 1);
    ASSERT_TRUE(ffsv_flash_count_matching(flash, FFSV_OP_READ, SIZE_MAX,
                SIZE_MAX, "beta") == 1);
    ASSERT_TRUE(ffsv_flash_count_matching(flash, FFSV_OP_READ, SIZE_MAX,
                SIZE_MAX, "alpha") == 0);

    FILE *tmp = tmpfile();
    ASSERT_TRUE(tmp != NULL);
    ASSERT_OK(ffsv_flash_dump_log(flash, tmp));
    rewind(tmp);
    ASSERT_TRUE(fread(buf, 1, sizeof(buf) - 1, tmp) > 0);
    ASSERT_TRUE(strstr(buf, "seq,type,offset,size,sector") != NULL);
    ASSERT_TRUE(strstr(buf, "alpha") != NULL);
    fclose(tmp);

    tmp = tmpfile();
    ASSERT_TRUE(tmp != NULL);
    ASSERT_OK(ffsv_flash_dump_timeline(flash, tmp));
    fclose(tmp);

    ffsv_flash_destroy(flash);
    return 0;
}

static int test_config_presets(void) {
    struct ffsv_flash_config cfg;
    struct ffsv_flash *flash = NULL;
    ASSERT_OK(ffsv_flash_config_preset(&cfg, FFSV_PRESET_TARGET_NOR_NOTES,
                4096 * 2));
    ASSERT_TRUE(cfg.sector_size == 4096);
    ASSERT_OK(ffsv_flash_create_with_preset(&flash,
                FFSV_PRESET_ESP32S3_MEASURED, 4096 * 2));
    ASSERT_OK(ffsv_flash_config_preset(&cfg, FFSV_PRESET_ESP32S3_MEASURED,
                4096 * 2));
    ASSERT_TRUE(cfg.program_granule == 16);
    ASSERT_TRUE(cfg.timing.erase_fixed_ns == 21269000);
    ffsv_flash_destroy(flash);
    ASSERT_EQ_INT(FFSV_ERR_INVALID,
            ffsv_flash_config_preset(&cfg, FFSV_PRESET_TARGET_NOR_NOTES, 4097));
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_nor_semantics_and_wear();
    failures += test_failure_matrix_program();
    failures += test_failure_matrix_erase_commit_and_corrupt();
    failures += test_drop_staged_before_after();
    failures += test_snapshot_reopen_and_image_load();
    failures += test_cross_sector_mutations_and_counts();
    failures += test_log_filter_validation_and_dumps();
    failures += test_config_presets();
    if (failures) {
        fprintf(stderr, "%d verify_flash tests failed\n", failures);
        return 1;
    }
    printf("verify_flash tests passed\n");
    return 0;
}
