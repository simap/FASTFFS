# FASTFFS - The Fast Atomic Sector Table Flash File System

***It's FAST, FFS!*** [[1]](#benchmark-snapshot)

FASTFFS is a small NOR flash filesystem prototype for embedded systems that
create, replace, list, and read named files. It is aimed at workloads with many
small to medium files, from a few bytes to tens of KB, a few larger files in the
hundreds of KB, and whole file updates rather than POSIX style random
write/update capabilities.

The target storage shape is roughly 2-12 MB of NOR flash with hundreds to low
thousands of files.

The core idea is simple: file data is written copy-on-write into flash sectors,
then a compact append-only index record commits the namespace update. On
remount, the index is replayed to recover the current set of names. Old file
versions and interrupted writes are left as obsolete or orphaned data for later
reclamation.

Like most flash filesystems FASTFFS is designed to be safe for use in the face
of unexpected power failures and crashes. It's designed to preserve committed
data across interrupted writes.

Unlike most flash filesystems, FASTFFS is designed to be fast. FASTFFS systems
usually run GC (garbage collection) opportunistically in small background steps,
keeping file operations as fast as possible. It still works without background
GC, but is much faster with it.

## Design Goals

1. Embedded / microcontroller environments: limited static RAM, lightweight code.
2. Safe, crash/power failure resilient, atomic updates of file data and metadata.
3. Fast, reduce file read/write overhead as much as possible.
4. Optimal for its designed workload.
5. Reasonable flash wear leveling. 
6. Usable in a variety of use cases, without trying to be everything.

## What Exists Now

- A portable C core API in `include/fastffs/fastffs.h`.
- Format, mount, open, read, write, close, stat, exists, list, delete, and
  directory iteration operations.
- Caller owned filesystem, file, directory, cache, scratch, and allocation-map
  state; the core does not allocate from the heap.
- A sector based flash format with index sectors, compact namespace records,
  file metadata, forward extent links, overwrite/delete commits, index
  rotation/compaction, small background GC steps, and inline GC under pressure.
- Packed small-file storage so tiny files do not each consume a whole sector.
- Configurable index cache modes, including no cache, hash table, and full table builds.
- A host verification flash backend derived from the LittleFS emulator, with
  NOR programming rules, operation accounting, fake timing, failure injection,
  crash/reopen snapshots, and image inspection support.
- Host tools for creating, loading, dumping, checking, probing, and sweeping
  FASTFFS images.
- ESP32 S3 benchmark harnesses used to compare candidate filesystems against
  JesFS, SPIFFS, and LittleFS.

## Near Future Goals

- Optional CRC32 protected file data + metadata.
- Multi file atomic transactions.
- Accelerated directory listing. FASTFFS already supports prefix filtering.
- Sector compaction of live data.
- Attribute and long name support.
- Append oriented workloads like logs, data loggers, etc.
- Key Value optimized workloads, like extra tiny files.

## Concepts

FASTFFS uses "sector" as its allocation, metadata, scan, and reclaim unit. The
default sector size is 4 KiB, with the encoded sector size stored in the index
headers so mounted images can discover the format.

The namespace index is an append-only log of small records. Each record stores a
collision resolved 16 bit slot and a head sector. A head of zero is a delete
tombstone; otherwise the head points at the sector containing the root metadata
for the current file version. Later records win during replay.

File replacement is intentionally commit oriented. New data and metadata are
written first, and the namespace changes only when the new index record is
appended. That keeps the old committed file visible if an interrupted write does
not reach the commit point.

## Build And Test

```sh
make test
make test-sanitize
```

Additional targets in the `Makefile` exercise cache variants, churn workloads,
timing probes, and crash sweeps.

See `design.md` for the deeper design notes.

## Benchmark Snapshot

This is a comparison of other permissively licensed filesystems that were reasonably easy to port/integrate. There are many others, but most appeared to be glued to a whole operating system.

In this benchmark, the default FASTFFS debt-GC configuration writes tiny files
about 7x faster than the fastest tested non-FASTFFS result, writes 50 KB files
about 2x faster, performs name probes tens of times faster, and completes the
churn workload about 1.5x faster than LittleFS and JesFS.

Even a minimal RAM configuration stays faster than the other tested filesystems
on the core 64 B-50 KiB file read/write and churn workloads, while remaining
competitive in other aspects.

Tested on ESP32-S3, ESP-IDF v6.0-beta2, 4 MB data partition.

FASTFFS default debt-GC runs measured GC steps between foreground operations;
that GC time is included in total churn runtime. This roughly simulates spending
some idle cycles on GC opportunistically, instead of forcing foreground writes to
pay the whole reclaim cost when free space is tight.

The churn benchmark writes about 8 MiB of creates/replaces/deletes while keeping about 2.4 MiB live, measuring mutation throughput during the run and final 123-file read/list behavior.

| Metric | FASTFFS default debt-GC | FASTFFS minimal debt-GC | [LittleFS](https://github.com/littlefs-project/littlefs) | [JesFS](https://github.com/joembedded/JesFs) | [SPIFFS](https://github.com/pellepl/spiffs) |
|---|---:|---:|---:|---:|---:|
| FS memory, base + open + stack | 4,872 B + 376 B + 1,156 B | 256 B + 184 B + 1,156 B | 1,672 B + 756 B + 1,452 B | 164 B + 28 B + 1,164 B | 3,540 B + 344 B + 1,248 B |
| Tiny-file overhead, 192 x 64 B | 16 B/file | 16 B/file | 448 B/file | 4,032 B/file | 438 B/file |
| Write 192 x 64 B | 20.8 KiB/s | 10.1 KiB/s | 0.59 KiB/s | 2.93 KiB/s | 0.27 KiB/s |
| Read 192 x 64 B total | 179.2 KiB/s | 35.6 KiB/s | 1.95 KiB/s | 5.21 KiB/s | 2.71 KiB/s |
| Read 192 x 64 B open/file | 178 us | 1.52 ms | 11.1 ms | 11.9 ms | 22.9 ms |
| Write 16 x 50 KiB | 174.6 KiB/s | 122.2 KiB/s | 91.8 KiB/s | 72.9 KiB/s | 65.8 KiB/s |
| Read 16 x 50 KiB total | 4,404 KiB/s | 4,357 KiB/s | 2,116 KiB/s | 1,278 KiB/s | 322.6 KiB/s |
| List 208 files | 33.3 ms | 111 ms | 306 ms | 26.4 ms | 123 ms |
| Existing-name probe | 164 us | 1.50 ms | 20.0 ms | 12.0 ms | 24.5 ms |
| Missing-name probe | 78 us | 2.36 ms | 16.6 ms | 26.4 ms | 121 ms |
| Churn total wall time | 107.281 s | 152.385 s | 158.943 s | 161.230 s | 755.656 s |
| Churn 10-20 KiB write | 139.8 KiB/s | 77.3 KiB/s | 51.2 KiB/s | 52.1 KiB/s | 11.5 KiB/s |
| Churn 20-60 KiB write | 155.7 KiB/s | 97.0 KiB/s | 71.8 KiB/s | 56.9 KiB/s | 11.6 KiB/s |
| Churn 350 KiB write | 159.3 KiB/s | 113.3 KiB/s | 78.7 KiB/s | 48.4 KiB/s | 7.32 KiB/s |
| Churn final cold list, 123 files | 18.7 ms | 281 ms | 302 ms | 17.8 ms | 116 ms |
| Churn cold read 350 KiB total | 4,532 KiB/s | 4,456 KiB/s | 2,380 KiB/s | 3,037 KiB/s | 509.6 KiB/s |

## Notes in no particular order

JesFS really punches above it's weight. It's simple and lightweight.

LittleFS has some really solid test/verification stuff, and has really improved over time. I've borrowed their flash emulation framework to verify FASTFFS.

Some design concepts were borrowed from Zephyr ZMS.
