/*
 * SPDX-License-Identifier: MIT
 */

#include "churn_model.h"

#include <stdio.h>
#include <string.h>

static uint32_t prng_next(bench_churn_model_t *model)
{
    model->state = model->state * 1664525u + 1013904223u;
    return model->state;
}

static uint32_t choose_churn_size(bench_churn_model_t *model,
                                  bench_churn_class_t *cls)
{
    uint32_t r = prng_next(model) % 1000u;
    if (r < 800u) {
        *cls = BENCH_CHURN_CLASS_SMALL;
        return (10u * 1024u) + (prng_next(model) % (10u * 1024u + 1u));
    }
    if (r < 950u) {
        *cls = BENCH_CHURN_CLASS_MEDIUM;
        return (20u * 1024u) + (prng_next(model) % (40u * 1024u + 1u));
    }
    *cls = BENCH_CHURN_CLASS_LARGE;
    return 350u * 1024u;
}

static int find_free_slot(const bench_churn_model_t *model)
{
    for (int i = 0; i < BENCH_CHURN_MAX_FILES; ++i) {
        if (!model->slots[i].live) {
            return i;
        }
    }
    return -1;
}

static int choose_live_slot(bench_churn_model_t *model)
{
    int live_count = 0;
    for (int i = 0; i < BENCH_CHURN_MAX_FILES; ++i) {
        if (model->slots[i].live && i != model->protected_large_slot) {
            live_count++;
        }
    }
    if (live_count == 0) {
        if (model->protected_large_slot >= 0 &&
            model->slots[model->protected_large_slot].live) {
            return model->protected_large_slot;
        }
        return -1;
    }

    int target = (int)(prng_next(model) % (uint32_t)live_count);
    for (int i = 0; i < BENCH_CHURN_MAX_FILES; ++i) {
        if (model->slots[i].live &&
            i != model->protected_large_slot &&
            target-- == 0) {
            return i;
        }
    }
    return -1;
}

static uint32_t delete_weight(const bench_churn_slot_t *slot)
{
    uint32_t weight = slot->size / 4096u;
    return weight == 0 ? 1 : weight;
}

static int choose_delete_slot(bench_churn_model_t *model)
{
    uint32_t total_weight = 0;
    for (int i = 0; i < BENCH_CHURN_MAX_FILES; ++i) {
        if (model->slots[i].live && i != model->protected_large_slot) {
            total_weight += delete_weight(&model->slots[i]);
        }
    }
    if (total_weight == 0) {
        if (model->protected_large_slot >= 0 &&
            model->slots[model->protected_large_slot].live) {
            return model->protected_large_slot;
        }
        return -1;
    }

    uint32_t target = prng_next(model) % total_weight;
    for (int i = 0; i < BENCH_CHURN_MAX_FILES; ++i) {
        if (!model->slots[i].live || i == model->protected_large_slot) {
            continue;
        }
        uint32_t weight = delete_weight(&model->slots[i]);
        if (target < weight) {
            return i;
        }
        target -= weight;
    }
    return -1;
}

static void fill_delete_event(const bench_churn_model_t *model,
                              bench_churn_event_t *event, int slot)
{
    memset(event, 0, sizeof(*event));
    event->type = BENCH_CHURN_EVENT_DELETE;
    event->slot = slot;
    event->cls = model->slots[slot].cls;
    event->size = model->slots[slot].size;
    snprintf(event->name, sizeof(event->name), "%s", model->slots[slot].name);
}

static void fill_write_event(const bench_churn_model_t *model,
                             bench_churn_event_t *event)
{
    const bench_churn_slot_t *slot = &model->slots[model->pending_slot];
    memset(event, 0, sizeof(*event));
    event->type = BENCH_CHURN_EVENT_WRITE;
    event->slot = model->pending_slot;
    event->cls = model->pending_cls;
    event->size = model->pending_size;
    event->old_size = model->pending_replacing ? slot->size : 0;
    event->write_seed = model->total_written ^ (uint32_t)model->pending_slot;
    event->replacing = model->pending_replacing != 0;
    if (event->replacing) {
        snprintf(event->name, sizeof(event->name), "%s", slot->name);
    } else {
        snprintf(event->name, sizeof(event->name), "w%04d.bin", model->pending_slot);
    }
}

static void sample_live_file_count(bench_churn_model_t *model)
{
    model->live_file_sum += model->live_file_count;
    model->live_file_samples++;
}

void bench_churn_model_init(bench_churn_model_t *model,
                            uint32_t seed,
                            uint32_t target_live_bytes,
                            uint32_t target_written_bytes,
                            uint32_t target_slack_bytes,
                            uint32_t force_large_after_bytes)
{
    memset(model, 0, sizeof(*model));
    model->seed = seed;
    model->state = seed;
    model->target_live_bytes = target_live_bytes;
    model->target_written_bytes = target_written_bytes;
    model->target_slack_bytes = target_slack_bytes;
    model->force_large_after_bytes = force_large_after_bytes;
    model->pending_slot = -1;
    model->protected_large_slot = -1;
}

bench_churn_event_type_t bench_churn_model_next(bench_churn_model_t *model,
                                                bench_churn_event_t *event)
{
    memset(event, 0, sizeof(*event));
    event->slot = -1;

    if (!model->pending_write) {
        if (model->total_written >= model->target_written_bytes) {
            event->type = BENCH_CHURN_EVENT_DONE;
            return event->type;
        }

        if (!model->forced_large_written &&
            model->total_written >= model->force_large_after_bytes) {
            model->pending_cls = BENCH_CHURN_CLASS_LARGE;
            model->pending_size = 350u * 1024u;
            model->forced_large_written = 1;
        } else {
            model->pending_size = choose_churn_size(model, &model->pending_cls);
        }
        model->pending_write = 1;
        model->optional_delete_checked = 0;
        model->slot_chosen = 0;
        model->pending_replacing = 0;
        model->pending_slot = -1;
    }

    while (model->live_bytes + model->pending_size >
           model->target_live_bytes + model->target_slack_bytes) {
        int del = choose_delete_slot(model);
        if (del < 0) {
            break;
        }
        fill_delete_event(model, event, del);
        return event->type;
    }

    if (!model->optional_delete_checked) {
        model->optional_delete_checked = 1;
        if (model->live_bytes > model->target_live_bytes &&
            (prng_next(model) & 7u) == 0u) {
            int del = choose_delete_slot(model);
            if (del >= 0) {
                fill_delete_event(model, event, del);
                return event->type;
            }
        }
    }

    if (!model->slot_chosen) {
        if ((prng_next(model) % 100u) < 25u) {
            int slot = choose_live_slot(model);
            if (slot >= 0) {
                model->pending_slot = slot;
                model->pending_replacing = 1;
            }
        }
        if (model->pending_slot < 0) {
            model->pending_slot = find_free_slot(model);
            model->pending_replacing = 0;
        }
        if (model->pending_slot < 0) {
            int del = choose_delete_slot(model);
            if (del >= 0) {
                model->pending_slot = del;
                model->pending_replacing = 0;
                fill_delete_event(model, event, del);
                return event->type;
            }
        }
        model->slot_chosen = 1;
    }

    if (model->pending_slot < 0) {
        event->type = BENCH_CHURN_EVENT_NO_SLOT;
        return event->type;
    }

    fill_write_event(model, event);
    return event->type;
}

void bench_churn_model_apply(bench_churn_model_t *model,
                             const bench_churn_event_t *event)
{
    bench_churn_slot_t *slot;

    switch (event->type) {
    case BENCH_CHURN_EVENT_DELETE:
        slot = &model->slots[event->slot];
        if (slot->live) {
            model->live_bytes -= slot->size;
            slot->live = 0;
            if (model->live_file_count > 0) {
                model->live_file_count--;
            }
            model->delete_count++;
        }
        sample_live_file_count(model);
        break;

    case BENCH_CHURN_EVENT_WRITE:
        slot = &model->slots[event->slot];
        if (event->replacing && slot->live) {
            model->live_bytes -= slot->size;
            model->replace_count++;
        } else {
            model->live_file_count++;
            model->create_count++;
        }
        slot->live = 1;
        slot->cls = event->cls;
        slot->size = event->size;
        slot->write_seed = event->write_seed;
        snprintf(slot->name, sizeof(slot->name), "%s", event->name);
        if (event->cls == BENCH_CHURN_CLASS_LARGE &&
            model->protected_large_slot < 0) {
            model->protected_large_slot = event->slot;
        }
        model->live_bytes += event->size;
        model->total_written += event->size;
        model->op_count++;
        model->pending_write = 0;
        model->pending_slot = -1;
        model->slot_chosen = 0;
        sample_live_file_count(model);
        break;

    default:
        break;
    }
}

const char *bench_churn_class_name(bench_churn_class_t cls)
{
    switch (cls) {
    case BENCH_CHURN_CLASS_SMALL:
        return "small_10_20k";
    case BENCH_CHURN_CLASS_MEDIUM:
        return "medium_20_60k";
    case BENCH_CHURN_CLASS_LARGE:
        return "large_350k";
    default:
        return "unknown";
    }
}
