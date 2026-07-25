# FASTFFS host tests

What lives here:

- `tests/littlefs`: the FASTFFS fork of the littlefs `emubd` test block device, BSD-3-Clause, from upstream `v2.11.2`. See `tests/littlefs/README.md`.
- `tests/test_verify_flash.c`: tests for the verification flash layer (`include/fastffs/verify_flash.h`, `src/verify_flash.c`): NOR monotonic program semantics, erase/read/blank checks, wear accounting, operation logs/counters, fake timing, deterministic failure injection, staged writes, corruption helpers, crash/reopen snapshots, and image dump/load/inspection. The layer's contract is documented in `docs/verify_flash_api.md`.
- `tests/test_fastffs.c`: filesystem tests against the verification flash backend via `include/fastffs/fastffs_host.h`: format, mount/discovery, streaming read/write/seek, stat/exists/list, overwrite/delete, allocation and reservations, GC and root-only compaction, index rotation/compaction, crash/power-loss recovery, and image inspection.
- `tests/fixtures`: directory-tree fixtures for the `fffs_tool` image load/check steps in `make test`.

Run the suite:

```sh
make test
make test-sanitize
```

`make test` runs both test binaries plus a crash sweep and `fffs_tool` workload/load/check passes. `make test-sanitize` repeats it under the sanitizer flags.

The default build uses the `(slot, head)` hash-table index cache (`FFFS_INDEX_CACHE_HASH_HEADS`, see `include/fastffs/fffs_opts.h`). Variant builds of the same suite:

```sh
make test-full-index      # FFFS_INDEX_CACHE_FULL_SLOT_HEADS direct 65K slot-head table
make test-nocache         # FFFS_INDEX_CACHE_NONE on-flash index scanning
make test-full-alloc-map  # explicit FFFS_ALLOC_MAP_FULL_BITMAP build
```

Longer-running targets include `test-churn*` for churn and small-file workloads, `test-timing*` for timing probes, `test-crash-sweep` and `test-api-crash-sweep` for crash/fault sweeps, and `test-compaction-stack-usage` for static stack accounting. See the `Makefile` for the full list.

For integrating FASTFFS into an application, see `INSTALLING.md`. `design.md` is the design reference.
