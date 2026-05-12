# FASTFFS Roadmap

The design document is authoritative. If this roadmap and `design.md`
disagree, `design.md` wins.

## Roadmap Shape

The initial work should move in layers:

1. **Build the verification groundwork.**
2. **Prove the format on a host image.**
3. **Make correctness executable with model and fault tests.**
4. **Bring it to ESP32-S3 hardware.**
5. **Benchmark against JesFS, SPIFFS, and LittleFS.**
6. **Optimize record variants, cache modes, and reclaim behavior.**

The main risk is not raw flash throughput. The main risks are on-flash format mistakes, crash-state mistakes, and GC accidentally reclaiming something reachable. The early roadmap should bias toward inspection tools and fault tests before performance tuning.

## Stage 1: Verification Groundwork

The first milestone is not a filesystem. It is a verification environment strong enough to make filesystem failures reproducible.

This stage builds an emulated NOR flash layer that can:

- enforce erased/programmed bit transitions
- track every read, program, erase, and blank-check operation
- preserve operation ordering and sequences
- count operations by type, size, FASTFFS sector, and call site
- model fake time using configurable flash timing stats
- expose deterministic failure injection
- simulate crashes and power loss before, after, or in the middle of operations
- stage partial mutations before committing them to the emulated flash image
- inject state mutation and corruption for recovery tests

The flash emulator should be reusable by normal host tests, model tests, fuzz/property tests, and future benchmark-style host runs. It should make failure cases inspectable: a failed test should be able to dump the operation log, flash image, fake-time timeline, and failure point.

The expected output is a standalone verification harness and fake flash backend that can test simple flash clients before FASTFFS exists.

The littlefs project has a fairly robust test setup. Lets start with that and lightly fork it as needed.

## Stage 2: Format and Host Prototype

The first milestone is a host-runnable filesystem image, not hardware.

This stage defines the exact binary layout for:

- index sector header with magic/version, index count/serial, sector size, and flags
- 4-byte index records
- baseline "does everything" metadata record
- local tombstone bit
- sector-local data/metadata layout

The host image should be self-describing through index headers and image size. Mount should be able to discover the FASTFFS sector size and active index sequence even if sector `0` is erased or tombstoned.

The implementation should include a fake NOR flash backend with the same monotonic write and erase behavior as real flash. That lets the format evolve quickly without device flashing overhead.

The embedded-facing core API must not allocate heap memory. When the core needs
persistent caches or scratch storage, the application provides those buffers;
host tools and tests can use dynamic allocation freely.

The expected output is a small host library and image file that can format, mount, create files, list files, read files, overwrite files, delete files, and remount successfully.

Current Stage 2 boundary:

- sector-size discovery and active index sequence selection are part of Stage 2
- basic index rotation/compaction is part of Stage 2
- the baseline metadata record must define valid/tombstone state bits
- packed sectors, multiple metadata records per sector, reverse metadata
  scanning, reusable orphan-sector allocation, continuation extents, and
  programming local tombstones for reclaim are allocator/GC maturity work

## Stage 3: Inspection and Recovery Tools

Before adding much complexity, build tools that make the image explain itself.

Needed tools:

- image dump: show index headers, index sectors, FASTFFS sectors, metadata records, tombstones, extents, and live namespace
- fsck/check: replay the index first, then validate current live namespace
  entries against root metadata and extent chains; classify sector metadata as
  live, obsolete/orphaned, tombstoned, erased, or corrupt, where obsolete and
  orphaned metadata are expected COW states rather than corruption
- workload generator: deterministic tiny/medium/large create, overwrite, delete, list, and read streams

These tools are part of the filesystem, not nice-to-have extras. FASTFFS depends on replay, orphan tolerance, and local metadata scanning; without inspection tools, debugging crash and GC behavior will be too slow.

## Stage 4: Crash Model and Fault Testing

Once the host prototype works for ordinary operations, connect it to the verification groundwork and make crash behavior executable.

The model is simple:

- the global index is the namespace authority
- old data remains live until a new index record commits
- data/metadata written before an index commit may become orphaned
- delete commits through a slot tombstone
- blank-check prevents stale allocation state from corrupting live data
- GC must never erase anything reachable from the current index

Fault tests should interrupt both between operations and inside operations:

- data writes
- metadata writes
- index appends
- index rotation
- index header valid/tombstone transitions
- erased sector `0` and index-header discovery
- conflicting index header version/count/sector-size data
- partial/clobbered index entries, including all-ones and all-zeros cases
- local tombstone updates
- erases
- failed data-sector writes that must not be committed through the index
- GC scans
- staged flash mutations

The expected output is confidence that any interrupted operation remounts into either the old committed state or the new committed state, with only reclaimable orphaned data left behind.

## Stage 5: Allocator, Sector Metadata, and GC Maturity

After basic correctness, the allocator, sector-local metadata, and GC need to
behave well under churn.

Allocator direction:

- first-available from `alloc_cursor`
- dense fill of usable FASTFFS sectors
- contiguous extension when adjacent sectors are available
- simple range check for "largest metadata record plus minimum useful data"
- reserve some metadata slack for tombstones and future amendment records
- reuse orphaned sectors that have valid footer/metadata state and enough erased
  free space for another file record
- blank-check before programming

Sector metadata direction:

- packed small files with multiple metadata records per sector
- linked continuation extents for files larger than one contiguous allocation
- reverse metadata scanning from the sector tail/footer
- local tombstone state encoded as a monotonic bit transition

GC direction:

- scan from `gc_cursor`
- classify sector-local metadata records against the replayed current index and
  reachable extent chains
- program local tombstone bits for obsolete, orphaned, and deleted records
- erase sectors only after all metadata records in the sector are tombstoned,
  invalid only in erased space, or otherwise reclaimable by policy

This stage should focus on sustained create/overwrite/delete workloads and low-space behavior. Sector compaction can remain future work unless churn shows that plain GC is not enough.

## Stage 6: Hardware Bring-Up

Only after the host image and crash tests are useful should FASTFFS move to ESP32-S3 flash.

Hardware work includes:

- ESP-IDF partition flash backend
- raw flash timing capture
- mount/format smoke tests
- create/list/open/read/write/delete smoke tests
- reset/power-loss style remount tests where practical

The hardware stage should preserve the same core library and use only a thin platform backend. If the core starts depending on ESP-IDF details, portability and host testing will suffer.

## Stage 7: Benchmark Lane

FASTFFS should be measured against the existing benchmark lanes:

- JesFS as the closest simple NOR filesystem competitor
- SPIFFS as an ESP-IDF NOR baseline with known GC cliffs
- LittleFS as the robust copy-on-write baseline

The first comparison should reuse the current workload shape:

- format/mount context
- 192 tiny 64-byte files
- 16 medium 50 KB files
- churn with 10-20 KB, 20-60 KB, and one large ~350 KB file
- warm and cold list/open/read measurements
- delete and write latency percentiles

Additional benchmarks:

- append-log growth and readback
- truncate/replace config-style writes
- seek/readback over medium and larger files

The goal is not just peak throughput. The important numbers are open/list latency, tiny-file storage overhead, write tail latency under churn, and cold recovery cost.

## Stage 8: Optimization and Variants

Only after baseline correctness and benchmark data should the compact variants be added.

Likely optimization lanes:

- tiny-file metadata record
- compact continuation record
- reverse tail/root metadata for append-oriented files
- metadata amendment records for fields such as next extent and total size
- index transactions with optional CRC records
- long-filename extension record
- optional metadata CRC
- optional file-data CRC
- metadata slack tuning
- index wear modeling / index-count tuning
- larger-MCU metadata cache mode
- low-RAM scan mode refinement
- sector compaction / defrag
- optional erase-count metadata and count-aware allocation

These variants should be judged against the baseline "does everything" record, not added preemptively. The risk is format complexity; the reward must be measurable storage, RAM, latency, or recovery improvement.

## First Implementation Cut

The first useful implementation should be deliberately narrow:

- verification harness and emulated flash first
- host fake flash only
- 16-bit slots
- default 4 KB FASTFFS sectors
- 8-byte index sector header
- self-describing host image
- caller-provided static buffer model for the core API
- max probe distance 50
- 32-byte filenames
- baseline metadata record only
- no CRC
- no index transactions
- no reverse-tail or amendment records
- default index cache
- create/overwrite/delete/list/read
- basic GC
- image dump tool

That is enough to validate the architecture before spending time on embedded integration or format variants.

## Later Questions

These should stay visible, but they do not need to block the first cut:

- exact compact metadata variants
- CRC policy
- sector compaction policy
- erase-count wear metadata
- lowest-RAM scan mode ergonomics
- long filename handling
- KV-style API shape
