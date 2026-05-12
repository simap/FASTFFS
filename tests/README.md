# FASTFFS verification tests

Stage 1 uses a FASTFFS fork of the littlefs `emubd` test block device:

- `tests/littlefs` is the derived BSD-3-Clause emulator fork from upstream
  littlefs `v2.11.2`.
- `include/fastffs/verify_flash.h` and `src/verify_flash.c` provide the
  FASTFFS-facing verification API on top of that fork.

Run the host tests with:

```sh
make test
make test-sanitize
```

The current harness covers NOR monotonic program semantics, erase/read/blank
checks, wear accounting from the littlefs emulator, operation logs/counters,
fake timing, deterministic failure injection, staged writes, direct corruption
helpers, crash/reopen snapshots, image dump/load/inspection helpers, cross-sector
mutations, realistic config presets, log filtering, and before/middle/after
failure matrices.

See `docs/verify_flash_api.md` for the Stage 1 API contract and the decision to
use explicit image dump/load instead of the upstream `emubd` live disk mirror.

Stage 2 starts in `include/fastffs/fastffs.h` and `src/fastffs.c`, with
`include/fastffs/fastffs_host.h` adapting the verification flash backend for
host tests. The core uses caller-owned mount state, caller-owned file state, and
caller-provided mount caches; it does not allocate from the heap. The default
index cache mode is a head-only hash table selected in
`include/fastffs/fffs_opts.h`; `make test-full-index` also builds the direct
65K slot-head table mode. The first host prototype test covers format, mount,
open with flags, streaming write/read, stat/fstat, exists, list, overwrite,
delete, and remount. See `docs/fastffs_host_api.md` for the implemented subset
and the deliberate gaps left for the next cuts.
