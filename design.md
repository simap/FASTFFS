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
- Erase flips a backend erase unit back to erased state.
- State transitions must be monotonic: committed/tombstoned values must be reachable by clearing bits only.
- Blank checks are cheap enough to use before allocation.
- Background erase is expected for sectors belonging only to dead/orphaned data.
- Flash wear is on erase cycles, not programming/overwrite cycles.
- Expected NOR endurance is around 100K erase cycles, so v1 should prefer simple rotating allocation over moving otherwise-stable data solely for static wear leveling.
- Flash will have an erasable unit that is some power of 2.
 
FASTFFS uses "sector" for its logical allocation, index, scan, and reclaim unit. This must be >= to the minimum backend flash erase unit. The sector size is encoded as `256 << sector_shift`; the default is 4 KB (`sector_shift = 4`).

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

Measured timing will vary by chip, driver, OS, and test harness. One ESP32-S3 built-in flash partition snapshot measured:

| Operation | Measured Time |
|---|---:|
| erase 4 KB sector | 21.269 ms |
| erase 64 KB range | 33.859 ms |
| read 4 bytes | 65 us |
| read 256 bytes | 89 us |
| read 4 KB | 554 us |
| program 256-byte page, avg | 1.043 ms |
| program 256-byte page, max | 5.552 ms |
| erase + program 4 KB, 256-byte pages | 45.587 ms |
| erase + program 4 KB, 1 KB chunks | 38.036 ms |

## Global Index

The global index is an append-only namespace journal. It is the authoritative source for file existence.

Conceptually, the namespace is a hash table using open addressing with bounded linear probing. Index records store the resolved hash slot and point to the sector where the file root metadata resides.

The probe rule is:

```text
base_slot = hash16(filename)
resolved_slot = base_slot + probe_distance
```

The probe distance is bounded by a configured maximum, defaulting to 50. If no free/resolved slot is available within that window, create/overwrite fails with a specific allocation or probe-limit error rather than continuing into an unbounded search. This keeps slot locality: a lookup or existence check only needs to consider resolved slots in:

```text
[hash16(filename), hash16(filename) + max_probe_distance]
```

If no occupied slot exists in that range, the file cannot exist. If occupied slots do exist in that range, the implementation reads the pointed root metadata records and checks the filename. With an in-RAM occupied-slot map, missing lookups do not need to read every possible slot in the window, only occupied candidate slots.

Example:

Lets say a given filename hashed to `abcd`. The index might look like this:

```text
abcd,1
abcd,2
abcd,0
abcd,3
```

Meaning:

- `abcd,1`: create/update file at slot `abcd`, head sector `1`
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
    uint16_t head;  // 0 = delete, otherwise sector containing file root metadata
};
```

The `slot` is not merely a raw hash. It is a resolved slot chosen by bounded probing. Full filename verification happens in out-of-line file metadata.

The `head` points to a FASTFFS sector, not an arbitrary byte, page, or backend erase unit. File root metadata is found by scanning the metadata records at the end of the head sector. This allows one head sector to contain multiple small files, one large file beginning, or a mix of both.

Index records do not carry per-record checksums. A record with all bits `1` is free space and marks the end of written records for that index sector. A record with all bits `0` is invalid/clobbered. Any other record is considered valid only if its `head` points to a valid sector containing valid root metadata whose resolved slot matches the record. If that check fails, corruption has occurred.

For an 8 MB flash with 256-byte pages:

- 32K pages maximum.
- 2K 4 KB erase sectors maximum.
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

With 4-byte records and the 8-byte v1 header in a 4 KB sector:

- a raw sector holds 1024 records
- reserving the header leaves 1022 file records per sector
- `n + 1` index sectors can store up to `n * 1022` compacted live entries while keeping one erased sector available for safe compaction
- at least two index sectors are needed, because erase happens a sector at a time

Version 1 reserves an 8-byte header at the start of each index sector:

```c
struct fffs_index_header_disk {
    uint8_t magic[4];      // "FFFS"
    uint8_t version;       // 1
    uint8_t index_meta;    // high nibble: index sector count, low nibble: serial
    uint8_t sector_shift;  // sector_size = 256 << sector_shift
    uint8_t flags;         // lifecycle and format policy bits
};
```

The index count is 2-15. Version 1 uses 16-bit slots, 16-bit sector heads, and 4-byte index records.

The `flags` byte uses cleared bits for state:

- clear `0x80`: header is committed/valid
- clear `0x40`: index sector is obsolete/tombstoned
- clear `0x20`: metadata CRC is required

Unknown cleared bits cause mount failure. Unknown bits still set are reserved.

Serial values only need to distinguish the active sequence of index sectors. The baseline uses a 4-bit serial, enough for up to 15 index sectors before ambiguity. Wrapping is fine when the sequence is known, e.g. `14 -> 15 -> 0 -> 1`. With 4 KB index sectors and 4-byte records, 15 sectors is about 15K index records.

Incremental rotation for `n = 2` live index sectors plus one spare:

```text
valid: 1, 2
free:  3
```

When sectors `1` and `2` are full:

1. Compact the oldest valid sector, `1`, into the erased/free sector, `3`.
2. Omit entries from `1` that are no longer current after replaying the full valid index sequence.
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

Only records from the oldest index sector are copied during a rotation, but the decision to copy or omit each record uses knowledge from the full replayed namespace across all valid index sectors.

If power fails before the new sector is marked valid, the old sequence remains authoritative. If power fails after the new sector is marked valid but before the old sector is tombstoned, the serial sequence identifies the newer valid set.

Index maintenance can run in the background like erase. Under pressure, a writer can fall back to on-demand rotation if it needs append space before the background task has freed any.

## Startup

On startup:

1. Scan index sector headers and select valid, non-obsolete sectors with compatible magic, version, and sector size.
2. Replay the index into RAM.
3. Later entries replace earlier entries.
4. Delete records remove earlier live entries.
5. Ambiguous/colliding slots can be resolved by reading the pointed file metadata and verifying filenames.

Cold start cost is bounded by reading the compact index plus any needed file-header probes.

Startup caching is configurable:

- Low-RAM mode can scan the index and read root metadata only when an operation needs it.
- Default mode caches the replayed index and enough occupied-slot state to make missing lookups cheap.
- Larger-MCU mode can cache live root metadata and/or extent lists to make `stat`, `ls`, open, and seek mostly RAM operations.

Examples:

- read one 4 KB index sector: ~405 us raw flash time
- read 100 file-header pages: `100 * 100 us = 10 ms`
- read 1,000 file-header pages: `1000 * 100 us = 100 ms`

This is still below measured SPIFFS open/list behavior that reached hundreds of ms to nearly 1 second on larger partitions.

An optional background blank check can rebuild/update in-memory free/used state after boot. If writing begins before this completes, allocation still blank-checks candidates before use.

## Free/Used Tracking

Free/used state is tracked at FASTFFS sector level with a bitmap.

The bitmap is an optimization, not the source of truth:

- It can be stale after a crash.
- Allocation checks candidate sectors for blank state before writing.
- If stale, the allocator may skip usable space temporarily.
- Successful writes can update the bitmap lazily.
- Full truth can be reconstructed from the index and sector metadata.

The allocator also keeps an `alloc_cursor`, the next sector to try for foreground allocation. Allocation is first-available from that cursor. New writes fill a usable sector densely until it no longer has enough free space for the largest supported metadata record plus the configured minimum useful data space. The allocator does not try to hunt for sparse holes before filling the current usable sector.

GC keeps a separate `gc_cursor`, the next sector to inspect for reclaim. Keeping these cursors separate lets foreground allocation, background reclaim, and wear distribution progress independently.

## Commit Order

File create/overwrite:

1. Allocate candidate sectors.
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
- Delete-by-name first resolves the name to its occupied slot using the normal lookup/probe rule, then appends that slot tombstone.

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

New files prefer a sector with enough free space for the largest supported metadata record plus at least a configured minimum threshold of file data. A reasonable starting threshold is 128-256 bytes. The exact formula is a tunable definition, but runtime allocation should be a simple range check.

## Sector Metadata

Sector metadata is out-of-line from the global index. It describes file data, extents, continuations, sizes, names, and local placement.

Metadata has multiple record variants with different storage costs. The default implementation can start with a "does everything" record that supports file heads, extents, continuations, tombstones, size, and the configured filename limit. Later variants can specialize for long filenames, tiny files with short inline names, compact continuation records, or lightweight key/value records. Some variants may be compile-time configuration; others can be selected at runtime based on the file shape.

The default file layout is linked single-extent metadata. Each metadata record describes one contiguous data extent. If a file continues into a non-contiguous sector, the current extent metadata links to the next extent's head sector.

Default root metadata should include:

- resolved slot
- filename, up to the default configured limit of 32 bytes
- sector-local data offset/length for this extent
- total file size for fast `stat`
- next extent head sector, if any
- local tombstone bit/state

Continuation metadata can be smaller:

- resolved slot
- sector-local data offset/length for this extent
- next extent head sector, if any
- local tombstone bit/state
- no filename
- no total file size unless needed for validation

Metadata records can be typed variable-length records, or a few compact fixed variants. Continuations do not need the full root metadata fields.

Continuation metadata still carries the resolved slot so GC can check liveness against the global index. A continuation from an overwritten file must not look live merely because the same slot was reused by a newer root.

Additional owner identity, such as root head or generation, is not part of the baseline unless GC needs it to avoid re-following from the index. The first implementation can validate continuation liveness by resolved slot plus the current index/root chain.

Metadata CRC support is optional, but if enabled it is a format-level policy advertised in index sector headers. A CRC-required image must not accept metadata records without valid CRC coverage as non-CRC records. CRC is not part of the 4-byte index record because that would destroy the compact index density.

## Local Tombstones

The global index delete is authoritative.

Sector-local tombstones are physical hints:

- help compaction/defrag
- allow metadata updates by writing a new metadata entry and tombstoning the old one
- are not required for namespace correctness

Tombstone state must be encoded as a monotonic NOR transition, e.g. valid -> obsolete by clearing one bit in the metadata record.

## Reads

Open for read:

1. Look up slot in RAM index.
2. Read head metadata.
3. Verify filename.
4. Return file handle.

Sequential reads follow linked extent metadata and should be close to raw flash speed.

Seek can follow the linked extent structure. If needed, cache the extent list after open or first seek.

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

The streaming write path does not need to know the final file length up front:

1. Open for write and find a sector with enough free space for metadata plus a minimum amount of data.
2. Blank-check and write data into the sector's data area.
3. If the next contiguous sector is usable, continue the same extent and defer writing the current extent metadata.
4. If allocation must jump to a non-contiguous sector, write the current extent metadata, pointing at the new extent head sector.
5. Repeat until close.
6. On close, write the final extent metadata with no next pointer.
7. Append the global index record last.

Metadata may be physically valid before the file is committed. Namespace visibility still comes only from the global index append. A crash before the index append leaves orphaned data/metadata for GC.

## Expected Performance

These estimates assume candidate sectors are already erased or can be blank-checked before use.

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

Sequential read/write overhead depends on the number of linked extent records and whether the open path caches an extent list. The design assumes open/read paths can cache enough file metadata to avoid repeated global scans.

## Bounded Linear Resolved Slots

Resolved slots are part of the baseline design.

This is different from re-hashing `filename + n` for each collision. Re-hashing can also bound the number of attempts, but candidate slots are scattered through the namespace, so the useful range filter is lost.

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

Duplicate raw hashes are not the baseline because they change delete and replay semantics: the index key is no longer unique without consulting metadata.

## Configuration Options

Several parts of the design can be optional or compile-time/runtime configuration choices:

| Option | Purpose |
|---|---|
| cache full file metadata at startup | Faster `stat`, `ls`, and open; uses more RAM |
| cache only index at startup | Lower RAM; open may read one metadata page |
| no preload / lazy metadata reads | Lowest startup work; cold operations may probe flash |
| background blank-check scan on boot | Refreshes in-memory free/used state before writes |
| skip boot scan and blank-check on allocation | Faster boot; bitmap may be stale until writes complete |
| sector size | Allocation/index/reclaim unit; encoded as `256 << sector_shift`, default 4 KB |
| backend erase unit | Runtime backend constraint; must divide FASTFFS sector size |
| minimum first-write threshold | Avoids starting files in sectors with too little data space |
| packed small files | Baseline density feature for tiny files |
| sector-local tombstones | Baseline physical hint for GC/defrag; not required for namespace correctness |
| linked single-extent metadata | Baseline continuation model |
| cache extent list on open or first seek | Makes later seeks faster; uses per-open RAM |
| background erase | Keeps write path fast; must be scheduled around frame/latency-sensitive work |

## Compaction and Reclaim

Index compaction:

- replay index
- emit only current live namespace entries
- omit obsolete updates and deletes

Sector reclaim:

- background task advances `gc_cursor` through sectors
- scans sector-local metadata records
- checks each valid-looking, non-tombstoned metadata record against the current index by resolved slot
- programs local tombstone bits for records that are deleted, overwritten, or orphaned
- erases sectors that contain only deleted/orphaned/tombstoned data
- advances allocation state so erased sectors can be found by the allocator

Sector-local compaction is TBD. It can behave like defrag: copy whole live files elsewhere, append normal overwrite records to the index, and allow the old sector to become reclaimable through ordinary GC. Because file size is known during compaction, it can try to choose contiguous sectors and reduce the number of extents. Flash does not materially care about sequential access, but contiguous placement benefits the linked-extent representation slightly by reducing metadata and seek traversal.

Wear leveling is intentionally simple in the baseline. Index rotation spreads index wear across the configured index sectors. The allocation cursor writes through unused/free sectors before wrapping, so with reasonable free space, data wear rotates through the partition. Static wear leveling by moving existing compact files is not planned for v1. If needed later, a non-file sector metadata record can store an erase counter; GC can update it after erase, and allocation can choose low-count sectors or a bounded low-count candidate near the cursor.

Local tombstones are hints for reclaim and compaction; the global index remains the authoritative namespace state.
