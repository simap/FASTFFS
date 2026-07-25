#!/usr/bin/env python3
"""Recompute the 16-bit resolved-slot collision table in design.md.

Simulates the FASTFFS resolved-slot rules from src/fffs_*index* and
tools/fffs_slot_resolution_probe.c:

- 65,536 slot values, 0x0000 and 0xffff excluded from resolution
- base slot from a uniform 16-bit hash, normalized away from the reserved values
- bounded linear probing upward with wraparound that skips reserved values

Columns:

- expected extra colliding names: live names whose resolved slot != base slot
- existing lookup probes: slots inspected from base to the name's resolved slot
- missing/new lookup probes: slots inspected until the first free slot for a
  random missing name, which is also the insert probe count for a new name

Usage: tools/slot_collision_table.py [insert_target] [seed]

insert_target sets total simulated inserts per row (trials = insert_target / n),
so small live counts get proportionally more trials.
"""

import random
import sys

SLOT_COUNT = 65536
RESERVED = (0x0000, 0xFFFF)
PROBE_LIMIT = 50
LIVE_COUNTS = (100, 500, 1000, 2000, 5000, 10000, 32768)
MISSING_SAMPLES = 2000000


def normalize_base(slot):
    if slot == 0x0000:
        return 0x0001
    if slot == 0xFFFF:
        return 0x7FFF
    return slot


def next_slot(slot):
    slot = (slot + 1) & 0xFFFF
    if slot in (0x0000, 0xFFFF):
        return 0x0001
    return slot


def run_trial(n, rng, missing_samples):
    occupied = bytearray(SLOT_COUNT)
    displaced = 0
    exist_probes = 0
    live = 0
    fails = 0
    while live < n:
        base = normalize_base(rng.getrandbits(16))
        slot = base
        for d in range(PROBE_LIMIT + 1):
            if not occupied[slot]:
                occupied[slot] = 1
                live += 1
                if d:
                    displaced += 1
                exist_probes += d + 1
                break
            slot = next_slot(slot)
        else:
            fails += 1

    miss_probes = 0
    for _ in range(missing_samples):
        slot = normalize_base(rng.getrandbits(16))
        probes = 1
        while occupied[slot]:
            probes += 1
            slot = next_slot(slot)
        miss_probes += probes

    return displaced, exist_probes / n, miss_probes, fails


def main():
    insert_target = int(sys.argv[1]) if len(sys.argv) > 1 else 5000000
    seed = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x46464653
    rng = random.Random(seed)

    print(f"slots={SLOT_COUNT} reserved={RESERVED} probe_limit={PROBE_LIMIT} "
          f"insert_target={insert_target} missing_samples={MISSING_SAMPLES} "
          f"seed={seed:#x}")
    print()
    print("| Live Files | Expected Extra Colliding Names | Extra as % of Files "
          "| Slot Load | Existing Lookup Probes | Missing/New Lookup Probes |")
    print("|---:|---:|---:|---:|---:|---:|")
    for n in LIVE_COUNTS:
        trials = max(30, insert_target // n)
        miss_trials = max(1, MISSING_SAMPLES // 100000)
        displaced = exist = miss = fails = miss_total = 0
        for t in range(trials):
            samples = 100000 if t < miss_trials else 0
            d, e, m, f = run_trial(n, rng, samples)
            displaced += d
            exist += e
            miss += m
            miss_total += samples
            fails += f
        displaced /= trials
        exist /= trials
        miss /= miss_total
        extra_pct = 100.0 * displaced / n
        load_pct = 100.0 * n / SLOT_COUNT
        disp_str = f"{displaced:,.2f}" if displaced < 100 else f"{displaced:,.1f}"
        print(f"| {n:,} | {disp_str} | {extra_pct:.2f}% | {load_pct:.2f}% "
              f"| ~{exist:.2f} | ~{miss:.2f} |")
        if fails:
            print(f"    (probe-limit insert failures across trials: {fails})")


if __name__ == "__main__":
    main()
