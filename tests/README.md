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
