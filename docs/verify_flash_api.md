# FASTFFS verification flash API

`include/fastffs/verify_flash.h` is the Stage 1 host verification contract.
It wraps the forked littlefs `lfs_emubd` backend and presents a byte-addressed
NOR flash model for ordinary tests, model tests, crash tests, fuzz/property
tests, and later host benchmarks.

## Geometry and presets

Create a device with `ffsv_flash_create()` and an explicit
`struct ffsv_flash_config`, or use `ffsv_flash_create_with_preset()`.

Available presets:

- `FFSV_PRESET_GENERIC_NOR`: 4 KiB sectors, 4-byte program granule,
  conservative SPI NOR timing.
- `FFSV_PRESET_ESP32S3_QIO`: 4 KiB sectors, 16-byte program granule,
  timing shaped for ESP32-S3 style QIO flash host tests.
- `FFSV_PRESET_SMALL_SPI_NOR`: 4 KiB sectors, byte program granule,
  slower timing for small serial NOR parts.

The emulator enforces range and alignment checks, monotonic NOR programming
(`1 -> 0` only), sector erases to `erased_value`, and erase wear counters.

## Operation logging and fake time

Every public operation records a monotonically increasing sequence number,
operation type, byte range, first sector, call site string, result, injected
failure phase, committed byte count, and fake time before/after the operation.

Use:

- `ffsv_flash_log()` for raw records.
- `ffsv_flash_counts()` for aggregate operation calls and bytes.
- `ffsv_flash_sector_counts()` for per-sector read/program/erase/blank-check
  accounting.
- `ffsv_flash_count_matching()` to validate specific log expectations by
  type, size, sector, and call site.
- `ffsv_flash_dump_log()` and `ffsv_flash_dump_timeline()` when a test needs
  durable failure evidence.

## Failure injection and crash modeling

`ffsv_flash_set_failure()` targets one future sequence number and operation
type mask. The failure can occur:

- `FFSV_FAIL_BEFORE`: no mutation commits.
- `FFSV_FAIL_MIDDLE`: a deterministic prefix commits and the call returns the
  injected status.
- `FFSV_FAIL_AFTER`: the full mutation commits and the call returns the
  injected status.

Program, erase, staged-commit, and corruption helpers report the committed
byte count in the operation log. Read and blank-check failures do not mutate
the image.

`ffsv_flash_stage_program()` validates and stores a pending program mutation
without changing the flash image. `ffsv_flash_commit_staged()` commits it with
the same before/middle/after failure semantics as direct programming.
`ffsv_flash_drop_staged()` can be used to model abandoned staged mutations.

## Crash/reopen snapshots

`ffsv_flash_snapshot_create()` captures the committed flash image, sector wear,
fake time, and next sequence number. `ffsv_flash_reopen_from_snapshot()` creates
a fresh emulator instance from that snapshot. The reopened device intentionally
starts with empty logs and no pending staged mutation; snapshots represent what
survived power loss, not in-process test runner state.

## Image load, dump, and inspection

Use `ffsv_flash_dump_image()` for durable binary images and
`ffsv_flash_load_image()` or `ffsv_flash_load_image_file()` to restore images
into a device with matching geometry. Image loads are raw test setup operations:
they replace the backend image directly and do not enforce NOR bit-transition
rules.

Use `ffsv_flash_image()`, `ffsv_flash_image_byte()`, and
`ffsv_flash_image_span_is_erased()` for assertions and inspection.

## Disk mirror decision

The underlying littlefs `emubd` has a disk mirror option, but FASTFFS Stage 1
uses explicit dump/load APIs instead. Dump/load is deterministic, portable
across tests, and easier to attach to failing test artifacts. A live mirror can
hide operation ordering and partial-mutation intent behind backend side effects,
so it remains an upstream backend feature rather than the FASTFFS verification
contract.
