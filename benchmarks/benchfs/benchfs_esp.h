/*
 * SPDX-License-Identifier: MIT
 */

#ifndef BENCHFS_ESP_H
#define BENCHFS_ESP_H

#include <stdint.h>

#include "benchfs.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t benchfs_esp_current_stack_used_bytes(void *ctx);
int benchfs_esp_run_noop_stack_baseline(void *ctx,
                                        const benchfs_config_t *cfg,
                                        uint32_t *used_bytes);

#ifdef __cplusplus
}
#endif

#endif
