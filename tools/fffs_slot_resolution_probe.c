/*
 * SPDX-License-Identifier: MIT
 *
 * FASTFFS bounded slot-resolution churn probe.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef FFFS_SLOT_RESOLUTION_LOG_FAILURES
#define FFFS_SLOT_RESOLUTION_LOG_FAILURES 0
#endif

enum {
    FFFS_SLOT_COUNT = 65536,
    FFFS_USABLE_SLOTS = 65534,
    NAME_ALPHABET = 26,
    MAX_NAME_LEN = 31,
    NAME_SIZE = MAX_NAME_LEN + 1,
    PROGRESS_SECONDS = 10,
    PROGRESS_CHECK_MASK = 0xfffff,
};

enum slot_source {
    SLOT_SOURCE_HASH,
    SLOT_SOURCE_RANDOM16,
};

static uint8_t slot_full[FFFS_SLOT_COUNT];
static uint16_t occupied_slots[FFFS_USABLE_SLOTS + 1u];

struct name_generator {
    uint8_t digits[MAX_NAME_LEN];
    uint8_t len;
};

static uint16_t fffs_hash16(const char *name) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return (uint16_t)((h >> 16) ^ h);
}

static uint16_t fffs_normalize_slot_base(uint16_t slot) {
    if (slot == 0) {
        return 1;
    }
    if (slot == UINT16_MAX) {
        return 0x7fff;
    }
    return slot;
}

static uint16_t fffs_next_slot(uint16_t slot) {
    slot++;
    if (slot == 0 || slot == UINT16_MAX) {
        return 1;
    }
    return slot;
}

static bool parse_u32(const char *s, uint32_t *out) {
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool next_name(struct name_generator *gen, char out[NAME_SIZE]) {
    if (gen->len == 0) {
        gen->len = 1;
    }

    for (uint8_t i = 0; i < gen->len; i++) {
        out[i] = (char)('a' + gen->digits[i]);
    }
    out[gen->len] = '\0';

    for (uint8_t i = gen->len; i > 0; i--) {
        uint8_t pos = (uint8_t)(i - 1u);
        if (gen->digits[pos] != NAME_ALPHABET - 1u) {
            gen->digits[pos]++;
            return true;
        }
        gen->digits[pos] = 0;
    }

    if (gen->len == MAX_NAME_LEN) {
        return false;
    }
    gen->len++;
    return true;
}

static uint64_t random_u64(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint16_t base_slot_for_name(enum slot_source source, uint64_t *rng,
        const char *name) {
    if (source == SLOT_SOURCE_RANDOM16) {
        return fffs_normalize_slot_base((uint16_t)random_u64(rng));
    }
    return fffs_normalize_slot_base(fffs_hash16(name));
}

static bool resolve_insert(uint16_t base_slot, uint32_t probe_limit,
        uint16_t *slot) {
    uint16_t candidate = base_slot;
    for (uint32_t d = 0; d <= probe_limit; d++) {
        if (!slot_full[candidate]) {
            slot_full[candidate] = 1;
            *slot = candidate;
            return true;
        }
        candidate = fffs_next_slot(candidate);
    }
    return false;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        fprintf(stderr,
                "usage: %s LIVE_COUNT PROBE_LIMIT [SEED] [hash|random16]\n",
                argv[0]);
        return 2;
    }

    uint32_t live_count;
    uint32_t probe_limit;
    uint32_t seed32 = 0;
    bool have_seed = false;
    enum slot_source source = SLOT_SOURCE_HASH;
    if (!parse_u32(argv[1], &live_count) ||
            !parse_u32(argv[2], &probe_limit)) {
        fprintf(stderr, "LIVE_COUNT and PROBE_LIMIT must be u32 values\n");
        return 2;
    }
    for (int i = 3; i < argc; i++) {
        uint32_t parsed_seed;
        if (parse_u32(argv[i], &parsed_seed)) {
            seed32 = parsed_seed;
            have_seed = true;
        } else if (strcmp(argv[i], "hash") == 0) {
            source = SLOT_SOURCE_HASH;
        } else if (strcmp(argv[i], "random16") == 0) {
            source = SLOT_SOURCE_RANDOM16;
        } else {
            fprintf(stderr, "expected SEED or slot source hash|random16\n");
            return 2;
        }
    }
    if (!have_seed) {
        seed32 = (uint32_t)time(NULL);
    }
    if (live_count > FFFS_USABLE_SLOTS) {
        fprintf(stderr, "LIVE_COUNT must be <= %u\n", FFFS_USABLE_SLOTS);
        return 2;
    }
    if (probe_limit >= FFFS_USABLE_SLOTS) {
        fprintf(stderr, "PROBE_LIMIT must be < %u\n", FFFS_USABLE_SLOTS);
        return 2;
    }

    uint64_t delete_rng = ((uint64_t)seed32 << 32) | seed32;
    uint64_t base_rng = delete_rng ^ 0xd1b54a32d192ed03ull;

    struct name_generator gen = {0};
    char name[NAME_SIZE] = {0};
    uint32_t occupied_count = 0;
    uint64_t prime_attempts = 0;
    uint64_t prime_failures = 0;
    while (occupied_count < live_count) {
        if (!next_name(&gen, name)) {
            fprintf(stderr, "filename sequence exhausted during priming\n");
            return 1;
        }
        prime_attempts++;

        uint16_t slot;
        uint16_t base_slot = base_slot_for_name(source, &base_rng, name);
        if (!resolve_insert(base_slot, probe_limit, &slot)) {
            prime_failures++;
#if FFFS_SLOT_RESOLUTION_LOG_FAILURES
            printf("prime_failure attempt=%llu slot=0x%04x filename=%s\n",
                    (unsigned long long)prime_attempts, base_slot, name);
            fflush(stdout);
#endif
            continue;
        }

        occupied_slots[occupied_count++] = slot;
    }

    printf("live=%u probe_limit=%u source=%s rng=splitmix64 "
            "alphabet=a-z seed=0x%08x "
            "prime_attempts=%llu prime_failures=%llu\n",
            live_count, probe_limit,
            source == SLOT_SOURCE_RANDOM16 ? "random16" : "hash",
            seed32,
            (unsigned long long)prime_attempts,
            (unsigned long long)prime_failures);
    fflush(stdout);

    uint64_t attempts = 0;
    uint64_t inserts = 0;
    uint64_t failures = 0;
    uint64_t last_attempts = 0;
    uint64_t last_failures = 0;
    time_t last_print = time(NULL);

    for (;;) {
        if (!next_name(&gen, name)) {
            fprintf(stderr, "filename sequence exhausted\n");
            return 1;
        }
        attempts++;

        uint16_t slot;
        uint16_t base_slot = base_slot_for_name(source, &base_rng, name);
        if (resolve_insert(base_slot, probe_limit, &slot)) {
            occupied_slots[occupied_count++] = slot;
            inserts++;
            uint32_t delete_pos = (uint32_t)(((random_u64(&delete_rng) >> 32) *
                    (uint64_t)occupied_count) >> 32);
            uint16_t delete_slot = occupied_slots[delete_pos];
            uint16_t last_slot = occupied_slots[occupied_count - 1u];
            slot_full[delete_slot] = 0;
            occupied_count--;
            occupied_slots[delete_pos] = last_slot;
        } else {
            failures++;
#if FFFS_SLOT_RESOLUTION_LOG_FAILURES
            printf("failure attempt=%llu slot=0x%04x filename=%s\n",
                    (unsigned long long)attempts, base_slot, name);
            fflush(stdout);
#endif
        }

        if ((attempts & PROGRESS_CHECK_MASK) == 0) {
            time_t now = time(NULL);
            if (now - last_print >= PROGRESS_SECONDS) {
                uint64_t delta_attempts = attempts - last_attempts;
                uint64_t delta_failures = failures - last_failures;
                double elapsed = (double)(now - last_print);
                double fail_prob = attempts == 0 ?
                    0.0 : (double)failures / (double)attempts;
                printf("attempts=%llu inserts=%llu failures=%llu "
                        "fail_prob=%.12g rate=%.0f/s recent_failures=%llu\n",
                        (unsigned long long)attempts,
                        (unsigned long long)inserts,
                        (unsigned long long)failures,
                        fail_prob,
                        (double)delta_attempts / elapsed,
                        (unsigned long long)delta_failures);
                fflush(stdout);
                last_attempts = attempts;
                last_failures = failures;
                last_print = now;
            }
        }
    }
}
