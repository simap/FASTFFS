# FASTFFS - The Fast Atomic Sector Table Flash File System

***It's FAST, FFS!***

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
5. Usable in a variety of use cases, without trying to be everything.

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
