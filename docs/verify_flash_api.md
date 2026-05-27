# FASTFFS Verify Flash API

`include/fastffs/verify_flash.h` defines the host-side flash verification
contract used by FASTFFS tests and crash sweeps. It wraps the FASTFFS fork of
littlefs `lfs_emubd` and exposes a byte-addressed NOR-like flash device to the
filesystem harness.

The split is intentional:

- `lfs_emubd` owns block-device state, copy-on-write block sharing, erase wear,
  and the low-level program/erase fault helpers added to the fork.
- `verify_flash` owns FASTFFS-facing byte offsets, alignment checks, timing
  accounting, operation logs, snapshots, targeted failure injection, and the
  online fault-sampling callback API.
- FASTFFS-specific semantic checks stay outside this layer. The flash emulator
  can present corrupted branches; it does not decide whether a FASTFFS mount,
  index replay, or file read is acceptable.

## Geometry

Create a device with `ffsv_flash_create()` and `struct ffsv_flash_config`, or
use `ffsv_flash_create_with_preset()`.

`ffsv_flash_config` describes:

- total image size
- erase sector size
- program and read granules
- erased byte value
- erase-cycle limit passed through to `emubd`
- fake timing coefficients
- maximum operation log entries

The config must describe an integral number of sectors, and sector size must be
divisible by both the program and read granules.

Two presets are available:

- `FFSV_PRESET_TARGET_NOR_NOTES`: default target NOR model, 4 KiB sectors,
  4-byte program granule, 1-byte read granule, `0xff` erased value, and timing
  values derived from the notes in `design.md`.
- `FFSV_PRESET_ESP32S3_MEASURED`: 4 KiB sectors, 16-byte program granule,
  1-byte read granule, `0xff` erased value, and timing values from the measured
  ESP32-S3 flash snapshot.

Tests may override fields after `ffsv_flash_config_preset()` and before
`ffsv_flash_create()`. `tools/fffs_api_crash_sweep` does this for configurable
sector size and image size.

## Operations

Public operations are byte-addressed:

- `ffsv_flash_read()`
- `ffsv_flash_program()`
- `ffsv_flash_erase()`
- `ffsv_flash_blank_check()`
- `ffsv_flash_stage_program()`
- `ffsv_flash_commit_staged()`
- `ffsv_flash_drop_staged()`
- `ffsv_flash_corrupt()`
- `ffsv_flash_xor()`

Reads and blank checks must be aligned to `read_granule`. Programs and staged
programs must be aligned to `program_granule`. Erases must be sector-aligned.

Normal program operations delegate to `lfs_emubd_prog()`, which programs by
ANDing bytes into the current block image. In other words, normal programming
can only clear bits. Raw corruption helpers are different: `ffsv_flash_corrupt()`
and `ffsv_flash_xor()` are test setup tools and can write arbitrary bytes.

`ffsv_flash_stage_program()` records one pending program mutation without
changing the image. `ffsv_flash_commit_staged()` commits that staged mutation
through the same program/failure/sampling machinery used by direct programs.
Only one staged mutation may exist at a time.

## Logs, Counts, And Time

Each public operation gets a monotonically increasing sequence number. The log
record stores:

- operation type
- byte range
- first sector
- call-site string
- fake time before and after the operation
- result code
- whether targeted failure injection matched
- injected failure phase
- committed byte count

The log starts small and grows up to `max_log_entries`; large log limits do not
force every temporary branch to allocate the full limit up front.

Use:

- `ffsv_flash_log()` to inspect log records.
- `ffsv_flash_counts()` for per-operation call and byte totals.
- `ffsv_flash_sector_counts()` for per-sector read/program/erase/blank-check
  accounting.
- `ffsv_flash_count_matching()` for tests that need to assert specific
  operation patterns.
- `ffsv_flash_dump_log()` for CSV-style durable logs.
- `ffsv_flash_dump_timeline()` for human-readable timing traces.

Fake time is deterministic. It is computed from the configured fixed and
per-byte timing coefficients; no wall-clock sleep is performed by
`verify_flash`.

## Targeted Failure Injection

`ffsv_flash_set_failure()` configures one targeted injected failure. Matching is
by future sequence number and operation type mask.

The supported phases are:

- `FFSV_FAIL_BEFORE`: return the injected status before any mutation commits.
- `FFSV_FAIL_MIDDLE`: commit a partial mutation, then return the injected
  status.
- `FFSV_FAIL_AFTER`: commit the full mutation, then return the injected status.

If `status` is zero, the injected status is `FFSV_ERR_INJECTED`.

For reads and blank checks, middle and after failures do not mutate anything.
The operation still returns the injected status after doing the normal read or
check work.

For program and staged-commit middle failures, `partial_bytes` selects the
maximum committed prefix. If `partial_bytes` is zero, the default is half the
operation, rounded down to the program granule, with at least one granule when
possible.

Program middle failures have two write modes:

- `FFSV_FAIL_WRITE_PREFIX`: program the committed prefix normally.
- `FFSV_FAIL_WRITE_RANDOM_CLEAR`: within the committed prefix, use the
  deterministic `emubd` random-clear helper. Only intended `1 -> 0` bit
  transitions are candidates. Stored `0` bits and unchanged `1` bits are not
  randomized.

`FFSV_FAIL_WRITE_RANDOM_CLEAR` is capped to the remaining bytes in a 256-byte
NOR program page and rounded to the program granule. The injected seed, sequence
number, offset, and size determine the exact result, so failures are
reproducible.

For erase middle failures, targeted injection still uses a sector-granular
prefix model. That is useful for deterministic before/middle/after crash-point
sweeps, but it is not the richer interrupted-erase model. Use the fault sampler
for randomized interrupted erase branches.

Corruption helpers also participate in targeted injection. A middle failure
applies a byte prefix of the requested corruption or XOR mask.

## Online Fault Sampling

`ffsv_flash_set_fault_sampler()` enables online branch sampling for mutating
operations. This is separate from targeted failure injection.

The sampler is meant for one-pass crash/fault exploration:

1. A real operation is about to run.
2. Before committing it, `verify_flash` creates one temporary copy-on-write
   branch per requested permutation.
3. The branch is mutated with one sampled partial-program or interrupted-erase
   outcome.
4. The caller's verification callback receives the branch and decides whether
   the filesystem state is acceptable.
5. The branch is destroyed.
6. If every callback returns `FFSV_OK`, the original operation commits normally
   on the real flash.

This avoids replaying the whole workload for every partial-write permutation.
The callback owns filesystem-specific verification. In the API crash sweep, the
callback remounts the branch and checks the expected namespace and file content.

`struct ffsv_fault_sampler` fields:

- `enabled`: enables sampling.
- `op_mask`: operation types to sample.
- `seed`: base PRNG seed; zero uses the built-in default.
- `permutations_per_op`: number of temporary branches per matching operation.
- `max_bits_per_permutation`: cap on sampled eligible program bits; zero means
  all eligible bits.
- `program_page_size`: program-page cap; zero means 256 bytes.
- `verify`: callback invoked for each branch.
- `user`: callback context.

The callback receives `struct ffsv_fault_case`:

- operation type, sequence, offset, and size
- permutation index and derived seed
- number of sampled program bits
- temporary `branch` flash handle
- an `ffsv_image_view` over the branch image

Current sampled program behavior:

- The fault span is capped to one program page and one sector.
- Candidate bits are exactly the bits that are stored as `1` and would become
  `0` if the operation fully programmed.
- Up to `max_bits_per_permutation` candidates are selected.
- For each selected candidate, the PRNG decides whether that bit remains `1`.
- The resulting partial program is applied to the temporary branch through
  `lfs_emubd_prog_random_clear()`.

Current sampled erase behavior:

- Each erased sector in the operation is replaced on the temporary branch with
  deterministic pseudo-random bytes.
- This deliberately does not model an orderly erased prefix. Interrupted erase
  can leave cells in undefined states, so the branch is treated as undefined
  data across the sampled sector.

Sampling is skipped when targeted failure injection already matched the real
operation. This keeps the two mechanisms independent: targeted injection mutates
the real image according to a selected sequence, while fault sampling explores
temporary branches before normal commit.

## Copy-On-Write Branches And Snapshots

`ffsv_flash_cow_clone()` creates an in-process copy-on-write clone using
`lfs_emubd_copy()`. The clone shares unchanged emubd blocks with the source and
copies only blocks that are later mutated. This is the preferred mechanism for
high-volume branch testing.

`ffsv_flash_snapshot_create()` creates a durable snapshot containing:

- flash config
- full image bytes
- per-sector wear
- next sequence number
- fake time

`ffsv_flash_reopen_from_snapshot()` creates a fresh device from that snapshot.
The reopened flash starts with empty logs and no staged mutation. Snapshots
represent what survived power loss; they do not preserve the test runner's
in-process state.

Use COW clones for fast branch exploration. Use snapshots when a test needs a
stable saved image, a reopen boundary, or a failure artifact that can be dumped
and reloaded.

## Image Access And Artifacts

Use these helpers for inspection and test artifacts:

- `ffsv_flash_image()` returns the current image cache.
- `ffsv_flash_image_byte()` reads one byte from the current image.
- `ffsv_flash_image_span_is_erased()` checks a span against the erased value.
- `ffsv_image_view_read()` and `ffsv_image_view_byte()` read an image view.
- `ffsv_flash_dump_image()` writes a raw image file.
- `ffsv_flash_load_image()` and `ffsv_flash_load_image_file()` replace the raw
  image with a supplied image.

Image load is a setup/recovery tool. It bypasses NOR programming rules and does
not replay operation semantics.

## Relationship To `lfs_emubd`

FASTFFS uses `emubd` as the underlying block device, not as a filesystem-aware
verifier. The important `emubd` features used here are:

- refcounted copy-on-write block copies via `lfs_emubd_copy()`
- erase wear counters
- normal program/erase/read operations
- deterministic program random-clear fault helper
- deterministic erase-random fault helper

`verify_flash` currently leaves `LFS_EMUBD_POWERLOSS_NOOP` configured for the
real backend. It does not use `LFS_EMUBD_POWERLOSS_OOO` as the crash model.
FASTFFS crash testing instead uses explicit targeted failure injection and
online COW branch sampling, because those APIs expose the exact operation,
sequence, image branch, and verification callback needed by the harness.

The `emubd` disk mirror is also not part of the FASTFFS verification contract.
FASTFFS uses explicit dump/load APIs for reproducible artifacts.
