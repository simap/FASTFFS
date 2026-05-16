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
- Configurable index cache modes, including no cache, hash table, and full table builds.
- A host verification flash backend derived from the LittleFS emulator, with
  NOR programming rules, operation accounting, fake timing, failure injection,
  crash/reopen snapshots, and image inspection support.
- Host tools for creating, loading, dumping, checking, probing, and sweeping
  FASTFFS images.
- ESP32 S3 benchmark harnesses used to compare candidate filesystems against
  JesFS, SPIFFS, and LittleFS.

## Near Future Goals

- Compact small file storage.
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

In this benchmark, FASTFFS writes tiny files 10x faster, writes 50 KB
files about 2x faster, lists directories 5x faster, performs name lookups over
160x faster, and can complete the churn workload about 1.5x faster than the
tested flash filesystems.

Tested on ESP32-S3, ESP-IDF v6.0-beta2, 4 MB data partition.

FASTFFS inline-GC performs no scheduled GC steps during the workload; GC runs on demand inside foreground mutation operations when needed. 
FASTFFS scheduled debt-GC uses the same filesystem configuration, but during churn runs measured GC steps between foreground operations; that GC time is included in total churn runtime. This roughly simulates spending some idle cycles on GC opportunistically.

The churn benchmark writes about 8 MiB of creates/replaces/deletes while keeping about 2.4 MiB live, measuring mutation throughput during the run and final 123-file read/list behavior.

| Metric | FASTFFS inline-GC | FASTFFS scheduled debt-GC | [LittleFS](https://github.com/littlefs-project/littlefs) | [JesFS](https://github.com/joembedded/JesFs) | [SPIFFS](https://github.com/pellepl/spiffs) |
|---|---:|---:|---:|---:|---:|
| FS RAM config | 2.8 KiB + 360 B/open | 2.8 KiB + 360 B/open | 1.4 KiB + 636 B/open | 164 B + 28 B/open | 532 B + 324 B/open slot |
| Tiny-file storage, 192 x 64 B | 4,138 B/file | 4,138 B/file | 554 B/file | 4,096 B/file | 502 B/file |
| Write 192 x 64 B | 20 KiB/s | 20 KiB/s | 0.6 KiB/s | 2 KiB/s | 0.2 KiB/s |
| Read 192 x 64 B total | 279 KiB/s | 279 KiB/s | 3 KiB/s | 4 KiB/s | 68.8 KiB/s |
| Read 192 x 64 B open/file | 78 us | 78 us | 9.61 ms | 12.6 ms | 509 us |
| Write 16 x 50 KiB | 179 KiB/s | 177 KiB/s | 92 KiB/s | 73 KiB/s | 65.8 KiB/s |
| Read 16 x 50 KiB total | 4,548 KiB/s | 4,547 KiB/s | 2,384 KiB/s | 1,237 KiB/s | 383 KiB/s |
| List 208 files | 15.3 ms | 15.3 ms | 277 ms | 78.8 ms | 133 ms |
| Existing-name probe | 76 us | 76 us | 17.6 ms | 12.2 ms | 51.1 ms |
| Missing-name probe | 63 us | 63 us | 15.3 ms | 27.2 ms | 131 ms |
| Churn total time | 153.4 s | 105.7 s | 173.4 s | 164.4 s | 826 s |
| Churn 10-20 KiB write | 57 KiB/s | 178 KiB/s | 51 KiB/s | 52 KiB/s | 10.6 KiB/s |
| Churn 20-60 KiB write | 57 KiB/s | 181 KiB/s | 60 KiB/s | 57 KiB/s | 11.5 KiB/s |
| Churn 350 KiB write | 36 KiB/s | 183 KiB/s | 79 KiB/s | 48 KiB/s | 6.5 KiB/s |
| Churn final cold list, 123 files | 9.15 ms | 9.14 ms | 400 ms | 74.3 ms | 126 ms |
| Churn cold read 350 KiB total | 4,564 KiB/s | 4,565 KiB/s | 2,431 KiB/s | 2,996 KiB/s | 454 KiB/s |

## Notes in no particular order

JesFS really punches above it's weight. It's simple and lightweight.

LittleFS has some really solid test/verification stuff, and has really improved over time. I've borrowed their flash emulation framework to verify FASTFFS.

Some design concepts were borrowed from Zephyr ZMS.
