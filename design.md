# FASTFFS Design

Design notes for a small NOR-flash filesystem optimized for named file replacement, fast listing/opening, crash consistency, and low metadata overhead.

## Design Workload

- 2-12 MB NOR flash.
- Hundreds to low thousands of files.
- Many tiny files, many files up to ~50 KB, a few larger files around ~300 KB.
- Common operations:
  - list files
  - read whole files
  - read file ranges
  - create or overwrite complete files
- Less important:
  - arbitrary in-place updates
  - POSIX-style seek/write/close behavior

## Flash Assumptions

- NOR erased state is all `1` bits, typically `0xff`.
- Programming only flips bits one direction, typically `1 -> 0`.
- Erase flips a whole sector back to erased state.
- State transitions must be monotonic: committed/tombstoned values must be reachable by clearing bits only.
- Blank checks are cheap enough to use before allocation.
- Background erase is expected for sectors belonging only to dead/orphaned data.
- Flash wear is on erase cycles, not programming/overwrite cycles. 

Measured / datasheet timing notes for the target NOR flash:

| Operation | Approx Time |
|---|---:|
| erase 4 KB sector | 36-56 ms, usually ~42 ms |
| erase 64 KB block | ~226 ms |
| read 4 bytes | ~74 us |
| read 256-byte page | ~100 us |
| read 4 KB | ~405 us |
| program 26 bytes | ~380 us |
| program 256-byte page | ~670 us |
| program 4 KB | ~9.1 ms |
| erase + program 4 KB | ~53 ms |
| blank check + program blank 4 KB | ~1.6 ms + ~9.1 ms |

An erase + program cycle is around 75 KB/s for 4 KB sectors. If sectors are erased ahead of time, checking and programming blank sectors is closer to 324 KB/s. Erases can stall animation/frame timing, so background erase should be scheduled carefully.

## Global Index

The global index is an append-only namespace journal. It is the authoritative source for file existence.

Conceptually, the namespace is a hash table using open addressing with linear probing. Index records store the resolved hash slot and point to the sector/page where the file root metadata resides.

Example:

Lets say a given filename hashed to `abcd`. The index might look like this:

```text
abcd,1
abcd,2
abcd,0
abcd,3
```

Meaning:

- `abcd,1`: create/update file at slot `abcd`, head sector/page `1`
- `abcd,2`: newer version
- `abcd,0`: delete tombstone
- `abcd,3`: recreated/newer version

When replayed, the final valid entry wins. During index compaction, obsolete earlier entries are omitted. If/when compacted, this would collapse to the last entry.

If a second file happened to have the same hash, a conflict would occur. The next available hash-slot is chosen by incrementing. e.g. the new file would get `abce`.

```text
abcd,1
abcd,2
abcd,0
abcd,3
abce,4
```

## Index Records

Compact default record:

```c
struct index_record {
    uint16_t slot;  // collision-resolved namespace slot
    uint16_t head;  // 0 = delete, otherwise sector/page containing file head metadata
};
```

The `slot` is not merely a raw hash. It is a resolved slot chosen by probing. Full filename verification happens in out-of-line file metadata.

For an 8 MB flash with 256-byte pages:

- 32K pages maximum.
- A 16-bit slot namespace is sufficient for the expected file count.
- At 100-1000 files, collision/probe cost is effectively negligible.
- Larger hash/slot widths can be a compile-time option for much larger namespaces.

Expected 16-bit collision/probe behavior:

| Live Files | Expected Extra Colliding Names | Extra as % of Files | Slot Load | Existing Lookup Probes | Missing/New Lookup Probes |
|---:|---:|---:|---:|---:|---:|
| 100 | 0.08 | 0.08% | 0.15% | ~1.00 | ~1.00 |
| 500 | 1.90 | 0.38% | 0.76% | ~1.00 | ~1.01 |
| 1,000 | 7.58 | 0.76% | 1.53% | ~1.01 | ~1.02 |
| 2,000 | 30.2 | 1.51% | 3.05% | ~1.02 | ~1.03 |
| 5,000 | 185.9 | 3.72% | 7.63% | ~1.04 | ~1.08 |
| 10,000 | 725.5 | 7.26% | 15.26% | ~1.08 | ~1.18 |
| 32,768 | 9,873.5 | 30.13% | 50.00% | ~1.39 | ~2.00 |

The last row is the physical maximum number of one-page files on an 8 MB flash before accounting for metadata, free space, obsolete versions, and GC headroom. For the expected hundreds-to-low-thousands file count, 16-bit slots keep the index compact while collision cost stays low.

## Index Rotation

The index can be treated as a circular buffer of index sectors. Compaction does not need to rebuild the whole namespace at once; it only needs to compact enough old index data to free append space.

With 4-byte records in a 4 KB sector:

- a raw sector holds 1024 records
- reserving the first record/header leaves 1023 file records per sector
- `n + 1` index sectors can store up to `n * 1023` compacted live entries while keeping one erased sector available for safe compaction
- at least two index sectors are needed, because erase happens a sector at a time

Each index sector should reserve a small header area, such as the first 4 bytes, for a combination of:

- magic/valid marker
- tombstone/obsolete marker
- serial/generation bits

Serial values only need to distinguish the active sequence of index sectors. A 4-bit serial can handle up to 15 index sectors; 16 would be ambiguous. Wrapping is fine when the sequence is known, e.g. `14 -> 15 -> 0 -> 1`.

Incremental rotation for `n = 2` live index sectors plus one spare:

```text
valid: 1, 2
free:  3
```

When sectors `1` and `2` are full:

1. Compact the oldest valid sector, `1`, into the erased/free sector, `3`.
2. Omit entries from `1` that were deleted or superseded by later entries in `1` or `2`.
3. Write the surviving entries into `3`.
4. Program the new sector header/magic last, with a serial newer than sector `2`.
5. Tombstone sector `1`.
6. Let background erase reclaim sector `1` when idle.

The valid sequence is now:

```text
valid: 2, 3
free:  1
```

The next rotation compacts effective live entries from sector `2` into erased sector `1`, with a serial newer than sector `3`.

If power fails before the new sector is marked valid, the old sequence remains authoritative. If power fails after the new sector is marked valid but before the old sector is tombstoned, the serial sequence identifies the newer valid set.

Index maintenance can run in the background like erase. Under pressure, a writer can fall back to on-demand rotation if it needs append space before the background task has freed any.

## Startup

On startup:

1. Replay the index into RAM.
2. Later entries replace earlier entries.
3. Delete records remove earlier live entries.
4. Ambiguous/colliding slots can be resolved by reading the pointed file metadata and verifying filenames.

Cold start cost is bounded by reading the compact index plus any needed file-header probes.

Examples:

- read one 4 KB index sector: ~405 us raw flash time
- read 100 file-header pages: `100 * 100 us = 10 ms`
- read 1,000 file-header pages: `1000 * 100 us = 100 ms`

This is still below measured SPIFFS open/list behavior that reached hundreds of ms to nearly 1 second on larger partitions.

An optional background blank check can rebuild/update in-memory free/used state after boot. If writing begins before this completes, allocation still blank-checks candidates before use.

## Free/Used Tracking

Free/used state is tracked at sector level with a bitmap.

The bitmap is an optimization, not the source of truth:

- It can be stale after a crash.
- Allocation checks candidate sectors/pages for blank state before writing.
- If stale, the allocator may skip usable space temporarily.
- Successful writes can update the bitmap lazily.
- Full truth can be reconstructed from the index and sector metadata.

## Commit Order

File create/overwrite:

1. Allocate candidate sectors/pages.
2. Blank-check before programming.
3. Write file data.
4. Write sector-local metadata.
5. Append the global index record last.

Crash behavior:

- Crash before index append: old version remains live; new data is orphaned.
- Crash after index append: new version is live.
- Crash before bitmap update: bitmap may be stale, but blank-check prevents corruption.

Delete:

- Append `slot,0` to the global index.
- This is authoritative even if old sector metadata remains.

## Sector-Local Layout

Sectors can pack data and metadata together:

```text
| data grows forward ... free space ... metadata grows backward |
```

Example:

```text
data________________md1
dataDDDDAAAATTTAAA_MD2md1
```

Small files can share a sector. Larger files can spill into continuation sectors. A continuation tail may still leave room for other file starts or small files.

## Sector Metadata

Sector metadata is out-of-line from the global index. It describes file data, continuations, sizes, names, and local placement.

Root metadata should include:

- resolved slot
- filename, up to a configured limit such as 32 bytes
- sector-local data offset/length
- total file size for fast `stat`
- next sector/page for continuation, if any

Continuation metadata can be smaller:

- sector-local data offset/length
- next sector/page
- no filename
- no total file size unless needed for validation

Metadata records can be typed variable-length records, or a few compact fixed variants. Continuations do not need the full root metadata fields.

## Local Tombstones

The global index delete is authoritative.

Sector-local tombstones are optional physical hints:

- help compaction/defrag
- allow metadata updates by writing a new metadata entry and tombstoning the old one
- are not required for namespace correctness

Tombstone state must be encoded as a monotonic NOR transition, e.g. valid -> obsolete by clearing bits only.

## Reads

Open for read:

1. Look up slot in RAM index.
2. Read head metadata.
3. Verify filename.
4. Return file handle.

Sequential reads follow sector/page links and should be close to raw flash speed.

Seek can follow the linked structure. If needed, cache the page/sector list after open or first seek.

## Writes

Write cost for blank pages is roughly:

```text
blank check + program data + local metadata write + one index append + bitmap update
```

Using measured approximate numbers:

- read/blank-check 256 bytes: ~100 us
- program 256 bytes: ~670 us
- tiny index write: <= ~380 us
- bitmap update: ~380-670 us

Erase cost is moved to a background task where possible.

## Expected Performance

These estimates assume candidate sectors/pages are already erased or can be blank-checked before use.

| Operation | Estimate |
|---|---:|
| read 4 KB index sector | ~405 us |
| probe 100 metadata/header pages | ~10 ms |
| probe 1,000 metadata/header pages | ~100 ms |
| open existing file, metadata not cached | ~100 us plus RAM lookup |
| open existing file, metadata cached | RAM lookup only |
| open missing file, low collision load | RAM lookup only, usually no flash read |
| sequential read 50 KB | roughly `50 KB / 4 KB * 405 us`, about ~5 ms raw read time |
| sequential read 300 KB | roughly `300 KB / 4 KB * 405 us`, about ~30 ms raw read time |
| write 50 KB to blank pages | `50 KB / 256 * 670 us`, about ~134 ms plus metadata/index/bitmap |
| write 300 KB to blank pages | `300 KB / 256 * 670 us`, about ~804 ms plus metadata/index/bitmap |
| program 4 KB blank sector | ~9.1 ms |
| erase + program 4 KB sector | ~53 ms |
| append one small index record | <= ~380 us |
| update bitmap page | ~380-670 us |

Sequential read/write overhead depends on whether the file is represented as linked pages, linked sectors, extents, or a cached page/sector list. The design assumes open/read paths can cache enough file metadata to avoid repeated global scans.

## Open Option: Bounded Linear Resolved Slots

One collision strategy is to keep the compact resolved-slot index record, but define the
probe rule explicitly:

```text
base_slot = hash16(filename)
resolved_slot = base_slot + probe_distance
```

The probe distance is bounded by a configured maximum. If no free/resolved slot is
available within that window, create/overwrite fails with a specific allocation or
probe-limit error rather than continuing into an unbounded search.

This keeps slot locality. A lookup or existence check only needs to consider resolved
slots in:

```text
[hash16(filename), hash16(filename) + max_probe_distance]
```

If no occupied slot exists in that range, the file cannot exist. If occupied slots do
exist in that range, the implementation reads the pointed root metadata records and
checks the filename. With an in-RAM occupied-slot map, missing lookups do not need to
read every possible slot in the window, only occupied candidate slots.

This is different from re-hashing `filename + n` for each collision. Re-hashing can
also bound the number of attempts, but candidate slots are scattered through the
namespace, so the useful range filter is lost.

A relatively large probe limit can still have acceptable cost. With a 16-bit slot
space and 10,000 live files, the slot load is about 15.3%. A one-direction inclusive
window of 51 slots (`max_probe_distance = 50`) has about 7.8 occupied candidate slots
on average for a random missing filename. If a root metadata probe is roughly one
256-byte flash read, this is around 0.8 ms of raw flash read time on the measured
target. Blindly checking all 51 possible slots would be around 5 ms, which is still a
bounded worst case.

Useful stats for this mode:

- maximum probe distance observed
- probe-limit failures
- metadata probes per lookup/open
- candidate slots skipped by the RAM occupied-slot filter

This remains an open option. The alternative is to allow duplicate raw hashes and use
sector metadata as the second part of the key. That avoids resolved-slot probing, but
changes delete and replay semantics because the index key is no longer unique.

## Configuration Options

Several parts of the design can be optional or compile-time/runtime configuration choices:

| Option | Purpose |
|---|---|
| cache full file metadata at startup | Faster `stat`, `ls`, and open; uses more RAM |
| cache only index at startup | Lower RAM; open may read one metadata page |
| no preload / lazy metadata reads | Lowest startup work; cold operations may probe flash |
| background blank-check scan on boot | Refreshes in-memory free/used state before writes |
| skip boot scan and blank-check on allocation | Faster boot; bitmap may be stale until writes complete |
| 16/24/32-bit slot width | Trade index density against large-namespace collision behavior |
| sector allocation only | Simpler free tracking and GC; small files may waste space |
| packed small files | Better density for tiny files; more sector-local metadata/compaction |
| sector-local tombstones | Helps local metadata updates and defrag; not required for namespace correctness |
| linked continuations | Simple write path and low metadata cost |
| extent metadata | Faster seek/range reads when files are mostly contiguous |
| cache page/sector list on open or first seek | Makes later seeks faster; uses per-open RAM |
| background erase | Keeps write path fast; must be scheduled around frame/latency-sensitive work |

## Compaction and Reclaim

Index compaction:

- replay index
- emit only current live namespace entries
- omit obsolete updates and deletes

Sector reclaim:

- background task scans sectors
- checks sector-local metadata against current index
- erases sectors that contain only deleted/orphaned data

Sector-local compaction can repack live small files/metadata into a fresh sector and then let the old sector be erased.
