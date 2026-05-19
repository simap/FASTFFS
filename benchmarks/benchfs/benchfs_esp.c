/*
 * SPDX-License-Identifier: MIT
 */

#include "benchfs_esp.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

typedef struct {
    const benchfs_config_t *cfg;
    TaskHandle_t waiter;
    uint32_t used_bytes;
    int rc;
} noop_stack_task_t;

static int64_t esp_now_us(void *ctx)
{
    (void)ctx;
    return esp_timer_get_time();
}

uint32_t benchfs_esp_current_stack_used_bytes(void *ctx)
{
    (void)ctx;
    uint32_t free_min = uxTaskGetStackHighWaterMark2(NULL);
    if (free_min >= CONFIG_ESP_MAIN_TASK_STACK_SIZE) {
        return 0;
    }
    return CONFIG_ESP_MAIN_TASK_STACK_SIZE - free_min;
}

static void noop_stack_task(void *arg)
{
    noop_stack_task_t *task = arg;
    task->rc = benchfs_run_noop(task->cfg, esp_now_us, NULL);
    task->used_bytes = benchfs_esp_current_stack_used_bytes(NULL);
    xTaskNotifyGive(task->waiter);
    vTaskDelete(NULL);
}

int benchfs_esp_run_noop_stack_baseline(void *ctx,
                                        const benchfs_config_t *cfg,
                                        uint32_t *used_bytes)
{
    (void)ctx;
    if (!cfg || !used_bytes) {
        return -1;
    }

    noop_stack_task_t task = {
        .cfg = cfg,
        .waiter = xTaskGetCurrentTaskHandle(),
    };
    BaseType_t ok = xTaskCreate(noop_stack_task, "benchfs_noop",
                                CONFIG_ESP_MAIN_TASK_STACK_SIZE, &task,
                                tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        return -1;
    }
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (task.rc != BENCHFS_OK) {
        return task.rc;
    }
    *used_bytes = task.used_bytes;
    return BENCHFS_OK;
}
