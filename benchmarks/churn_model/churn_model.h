/*
 * SPDX-License-Identifier: MIT
 *
 * Deterministic benchmark churn generator shared by filesystem harnesses.
 */

#ifndef BENCH_CHURN_MODEL_H
#define BENCH_CHURN_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BENCH_CHURN_MAX_FILES 256
#define BENCH_CHURN_NAME_LEN 24

typedef enum {
    BENCH_CHURN_CLASS_SMALL = 0,
    BENCH_CHURN_CLASS_MEDIUM = 1,
    BENCH_CHURN_CLASS_LARGE = 2,
    BENCH_CHURN_CLASS_COUNT = 3,
} bench_churn_class_t;

typedef enum {
    BENCH_CHURN_EVENT_DONE = 0,
    BENCH_CHURN_EVENT_DELETE = 1,
    BENCH_CHURN_EVENT_WRITE = 2,
    BENCH_CHURN_EVENT_NO_SLOT = 3,
} bench_churn_event_type_t;

typedef struct {
    uint8_t live;
    bench_churn_class_t cls;
    uint32_t size;
    char name[BENCH_CHURN_NAME_LEN];
} bench_churn_slot_t;

typedef struct {
    bench_churn_event_type_t type;
    int slot;
    bench_churn_class_t cls;
    uint32_t size;
    uint32_t old_size;
    uint32_t write_seed;
    bool replacing;
    char name[BENCH_CHURN_NAME_LEN];
} bench_churn_event_t;

typedef struct {
    uint32_t seed;
    uint32_t state;
    uint32_t target_live_bytes;
    uint32_t target_written_bytes;
    uint32_t target_slack_bytes;
    uint32_t force_large_after_bytes;

    uint32_t total_written;
    uint32_t live_bytes;
    uint32_t op_count;
    uint32_t create_count;
    uint32_t replace_count;
    uint32_t delete_count;
    uint32_t forced_large_written;
    uint32_t live_file_count;
    uint32_t live_file_samples;
    uint64_t live_file_sum;

    uint8_t pending_write;
    uint8_t optional_delete_checked;
    uint8_t slot_chosen;
    uint8_t pending_replacing;
    int pending_slot;
    int protected_large_slot;
    bench_churn_class_t pending_cls;
    uint32_t pending_size;

    bench_churn_slot_t slots[BENCH_CHURN_MAX_FILES];
} bench_churn_model_t;

void bench_churn_model_init(bench_churn_model_t *model,
                            uint32_t seed,
                            uint32_t target_live_bytes,
                            uint32_t target_written_bytes,
                            uint32_t target_slack_bytes,
                            uint32_t force_large_after_bytes);

bench_churn_event_type_t bench_churn_model_next(bench_churn_model_t *model,
                                                bench_churn_event_t *event);

void bench_churn_model_apply(bench_churn_model_t *model,
                             const bench_churn_event_t *event);

const char *bench_churn_class_name(bench_churn_class_t cls);

#ifdef __cplusplus
}
#endif

#endif
