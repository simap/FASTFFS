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

Index records do not carry per-record checksums. A record with all bits `1` is free space and marks the end of written records for that index sector. A record with all bits `0` is invalid/clobbered. Any other record is presumed valid. File lookup/exists are a higher standard and would only show an existing file if its `head` points to a valid sector containing valid root metadata whose resolved slot matches the record. If that check fails, corruption has occurred.

Version 1 stores index record fields as little-endian integers. `head == 0` is a delete tombstone. Nonzero heads must point outside the index sector range and below the derived sector count. `slot == 0xffff` remains legal unless the whole record is erased.

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

The index count is 2-15. Version 1 uses 16-bit slots, 16-bit sector heads, and 4-byte index records. The encoded sector size applies to index sectors and data sectors. All valid index sectors in one image must agree on version, index count, and sector shift.

The `flags` byte uses cleared bits for state:

- clear `0x80`: header is committed/valid
- clear `0x40`: index sector is tombstoned
- clear `0x20`: metadata CRC is required

Unknown cleared bits cause mount failure. Unknown bits still set are reserved.

Serial values only need to distinguish the active sequence of index sectors. The baseline uses a 4-bit serial, enough for up to 15 index sectors before ambiguity. Wrapping is fine when the sequence is known, e.g. `14 -> 15 -> 0 -> 1`. With 4 KB index sectors and 4-byte records, 15 sectors is about 15K index records.

Incremental rotation for `n = 2` live index sectors plus one spare:

```text
valid: 1, 2
free:  3
```

Rotation attempts to move the index head to the next sector while maintaining at least 1 free sector. This can trigger a compaction when no sector would be free. It's possible to rotate before completely filling a sector. For example we may want to reserve space for a maximum tx size and/or to reserve some usable index space during a background compaction.

Compaction can also be done independently. This is mostly a future concern. For example, we may notice that an index contains many obsolete records during index replay. Perhaps some simple heuristics like a minimum number of obsolete records or a percentage would trigger a compaction. Compaction would then likely reduce the total number of records that need to be replayed in the future. For memory backed indexes this may not be worth it, but for a non-caching lookup, the total cache size increases lookup time.

When compacting a sector, compact the oldest valid sector, `1`, into the current sector `2` (if there is room) and any overflow to the erased/free sector, `3`: 
   1. Start reading entries from the oldest sector `1`.
   2. Omit entries from `1` that are no longer current. Existing slot + head pairs that match current data from index replay are copied forward. Deletes do not get carried forward, since they can only impact the validity of past events.
   3. Write the surviving entries into `2` and overlowing into `3` when `2` is full.
   4. Program the new sector header/magic in `3` last, with a serial newer than sector `2`.
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

Index transactions are deferred. A likely design is to reserve `head == 1` as a control-record marker, since `head == 0` is a delete tombstone and sector `1` is always inside the index range. Transaction begin/end metadata can be encoded in `slot`; full CRC mode can burn one following 32-bit record for the CRC. That keeps ordinary records unchanged while allowing atomic multi-record updates later.
This would allow multiple index changes to be atomic, and CRC protected. Similar to LittleFS metadata commit batches.

It also allows for a CRC-backed index update even for single 
records, though would consume 4 records worth of index space.

## Startup

On startup:

1. If not given in mount data, discover the sector size by scanning for valid index headers at plausible `256 << sector_shift` boundaries, so mount can recover even when sector `0` is erased.
2. Select valid, non-tombstoned index sectors whose headers agree on magic, version, index count, and sector shift.
3. Replay the index into RAM.
4. Later entries replace earlier entries.
5. Delete records remove earlier live entries.
6. Ambiguous/colliding slots can be resolved by reading the pointed file metadata and verifying filenames.

Mount discovery is bounded to plausible index-sector header locations. When the sector size is not supplied by mount options, the
implementation probes candidate sector sizes in this order:

1. The default 4 KB sector size.
2. Smaller sector sizes down to 256 bytes.
3. Larger sector sizes up to the configured maximum.

For each candidate `sector_shift`, the candidate sector size is:

```text
sector_size = 256 << sector_shift
```

Discovery reads only the first two possible index-sector header locations for
that candidate size:

```text
offset = 0 * sector_size
offset = 1 * sector_size
```

Checking 2 index sectors is enough for normal recovery because the
format keeps at most one index sector erased. A header
found at a candidate offset is only usable for that candidate if the header's
encoded `sector_shift` matches the `sector_shift` currently being probed.

A usable candidate is not accepted immediately. The implementation then reads
the index-sector headers declared by that candidate's `index_count` and
`sector_shift`. Valid, non-tombstoned headers must agree on magic, version,
index count, and sector shift. If this validation fails, discovery continues
with the next candidate sector size. If validation succeeds, the newest valid
index sector is selected by serial and replay begins from that index sequence.

Format does not need to erase the entire filesystem area. To avoid stale index
headers from an older format being discovered after a sector-size change, format
erases:

```text
min(max(8192, sector_size * index_count), filesystem_size)
```

before writing the fresh index header at offset `0`. This clears the discovery
window for 256-byte through 4 KB sector sizes and clears every index sector in
the newly formatted geometry. Clearing the full new index area prevents stale
headers from an older format with a different index count from making candidate
validation fail. If a new format is interrupted before the fresh header is
valid, mount may still discover an older larger-format header; that is treated
as a failed format rather than a normal recovery path.

Cold start cost is bounded by reading the compact index plus any needed file-header probes.

Startup caching is configurable:

- Low-RAM mode can scan the index and read root metadata only when an operation needs it.
- Default mode caches the replayed index and enough occupied-slot state to make missing lookups cheap.
- Larger-MCU mode can cache live root metadata and/or extent lists to make `stat`, `ls`, open, and seek mostly RAM operations.

The embedded core should be usable without hidden heap allocation. Mount should take caller-provided buffers/caches sized from explicit configuration and decoded format limits. Host tools may use dynamic allocation freely.

Mount also takes a caller-provided global scratch buffer. The core must work
with a small buffer, but larger scratch improves operations that scan
flash ranges, such as blank-checking an allocation candidate. A scratch buffer
as large as the FASTFFS sector size lets the core blank-check a full sector with
one backend read instead of many small reads.

Examples:

- read one 4 KB index sector: ~405 us raw flash time
- read 100 file-header pages: `100 * 100 us = 10 ms`
- read 1,000 file-header pages: `1000 * 100 us = 100 ms`

This is still below measured SPIFFS open/list behavior that reached hundreds of ms to nearly 1 second on larger partitions.

An optional background blank check can rebuild/update in-memory free/used state after boot. If writing begins before this completes, allocation still blank-checks candidates before use.

## Optional Directory Objects as Secondary Index

FASTFFS can support prefix/directory-style listing without making hierarchy part
of the baseline namespace lookup. The baseline global index remains
authoritative:

```text
hash("cfg/")     -> directory object head
hash("cfg/net")  -> file object head
hash("cfg/ui")   -> file object head
```

In this model, a directory is a special object stored and committed like a file,
but its payload is a compact secondary index for children under that prefix.
The normal full-path index is still used for open, stat, delete, and liveness.
The directory object only accelerates listing and gives the API a natural place
to simulate or later expose hierarchy.

A simple directory payload can be an append-oriented array of resolved child
slots.

Entry values are monotonic NOR states:

- `0xffff`: empty / never written
- `0x0000`: removed / tombstoned entry
- `0x0001..0xfffe`: resolved child slot

Slot resolution should avoid `0x0000` and `0xffff`.
Those sentinels are only in the slot namespace; they are distinct from special
head-sector values such as `head == 0` for delete and any future `head == 1`
control record.

Directory listing then becomes:

1. Resolve/open the directory object for the requested prefix, such as `"cfg/"`.
2. Stream its `child_slot[]` entries.
3. Skip `0x0000` stop at `0xffff`.
4. Resolve each live child slot through the main index.
5. Read child root metadata and return entries whose names still belong to the
   directory prefix.

The main index remains the source of truth. If a child slot is missing from the
main index, points at deleted metadata, or resolves to a different name after
churn, the directory entry is stale. Normal operation can skip stale entries;
fsck/check should report them and directory compaction can remove them.

Directory entries are append/tombstone oriented. A removed child entry can be
programmed from its slot value to `0x0000` to remove it. 

Eventually the directory object either grows to another sector/extent or is
compacted copy-on-write into a new object and republished through the main
index.

This is deliberately a secondary index, not necessarily true hierarchy:

- Full-path file lookup can remain one hash/probe lookup.
- Directory objects can be compile-time optional.
- Filesystems without directory objects can still list by scanning/filtering the
  replayed global index.
- A future true-hierarchy mode can build on the same object type by making path
  lookup walk directory objects component by component.

Directory updates have transaction implications. Creating `"cfg/net"` when
`"cfg/"` already exists logically needs two namespace effects: publish the file
in the main index and append the child slot to the directory object. 

For consistency, it should be written to the directory first, then the main index to become real/valid. Otherwise the file would exist, but would be orphaned from directory listings. A file in a directory that isn't in the index is invalid and would be ignored.

Creating a
missing directory at the same time adds another index record. Without index
transactions, the directory should be created and populated first. With transactions, a create can atomically publish:

- any newly-created directory object
- the file's main index record
- the directory object's updated head, if the directory was created, grew, or
  was compacted copy-on-write

If an existing directory object can append the child slot in place without
moving its head, the main index entry for that directory does not need to be
rewritten. If the directory grows through linked extents under a stable head,
growth may also avoid a directory index update. If growth or compaction
publishes a replacement head, that directory index update should participate in
the same transaction as the related file operation when atomic directory
contents are required.

This should be an optional feature. It is most useful when a
workload performs frequent directory listings over a large namespace. Direct full path access wouldn't be impacted.

## Free/Used Tracking

Free/used state can be tracked at FASTFFS sector level with a bitmap. The
baseline can also be built with no allocation map, in which case allocation and
GC use linear scans plus blank checks and metadata classification. A full bitmap
uses caller-provided memory because it scales with sector count; an 8 MB
filesystem with 4 KB sectors needs 256 bytes. Smaller coarse maps can be
compile-time variants with inline storage.

The bitmap is an optimization, not the source of truth:

- It can be stale after a crash.
- Allocation checks candidate sectors for blank state before writing.
- If stale, the allocator may skip usable space temporarily.
- Successful writes can update the bitmap lazily.
- Full truth can be reconstructed from the index and sector metadata.
- A failed data-sector erase/program can be skipped without committing an index record pointing at it. Persistent bad-sector tracking can remain a later backend feature.

The full bitmap uses strict known-used semantics: `1` means known used,
live, or reserved; `0` means unknown or worth inspecting. A `0` bit is not
proof of free space, and a stale `1` must not be allowed to starve pressure
paths forever. Deletes clear the old file's known sector chain back to unknown
even in lazy tombstone mode, so GC can later reclassify those sectors without
delete-time sector-local flash programs.

A full bitmap does require a decent chunk of memory. With 4K sectors, you'd need 256 bytes to cover an 8MB filesystem.

A coarse bitmap could compress this down significiantly, while still having some benefit and avoiding scanning some areas that are full for alloc or GC. For example, a 64 bit wide map could cover an 8MB filesystem using 32 sector wide buckets per bit.

Coarse map variants should use conservative "proven full bucket" semantics,
not "some live sector was full" semantics. Since allocation attempts to fill sequentially, there should be many contiguous, completely full blocks of sectors that can be skipped over. There would still be some partially full buckets, but it would still be less scanning that scanning every sector.

The allocator also keeps an `alloc_cursor`, the next sector to try for foreground allocation. Allocation is first-available from that cursor. New writes fill a usable sector densely until it no longer has enough free space for the largest supported metadata record plus the configured minimum useful data space. The allocator does not try to hunt for sparse holes before filling the current usable sector.

GC keeps a separate `gc_cursor`, the next sector to inspect for reclaim. Keeping these cursors separate lets foreground allocation, background reclaim, and wear distribution progress independently.

The `alloc_cursor`, `gc_cursor`, and bitmap are reconstructable allocator hints,
not namespace state. They do not need to be updated atomically with file
commits. Losing them after power loss may cost scan time, not correctness.

The index tail provides a cheap allocator recovery hint. Writes try to fill
sectors, and each committed file update appends an index record pointing at the
head sector that was just written. During mount, the newest (tail) valid index records
therefore identify the sector region where allocation was happening at the last
committed write. Reading sector footers for those tail heads gives the newest
known sector serial and a good place to resume `alloc_cursor`.

It's possible some writes occured and were not commited, so scan should also look ahead a few sectors from the last commited one.

The sector serial is also a relative age signal for GC and wear distribution.
Low serial sectors are older allocation candidates. 

A static wear leveling system could move very old and stable files off of sectors with low serials. It would have to copy off every file, update the index, then reclaim the sector, so this add some work to the system. That might be desirable for flash systems with low program/erase cycle ratings that have a mix of stable and volatile file workloads. This is not implemented, but could be added later using existing FS structures.


## Commit Order

File create/overwrite:

1. Allocate candidate sectors.
2. Blank-check before programming.
3. Write file data.
4. Write sector-local metadata and the sector footer.
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

Allocator policy should also reserve some metadata slack for later tombstones and amendment records. The exact reserve is TBD. Might be configurable, with reserve = 0 effectively disables MD ammendment records.

## Sector Footer

Each data/mixed FASTFFS sector carries a small footer at the physical end of the
sector. The footer identifies the sector as FASTFFS-owned and records when the
sector was allocated or first written after erase.

The footer lives at the end rather than the beginning because metadata scanning
already reads from the sector tail. A single 128-byte or 256-byte read from the
end can fetch the footer plus the newest metadata records for mount, fsck, and
GC.

The first metadata record written into an erased sector claims that sector. Since
the tail-most metadata record is adjacent to the footer, that first metadata
write should program the metadata record and footer together as one contiguous
write. Later metadata records in the same sector are written farther toward the
front of the sector as metadata grows backward; they do not rewrite the footer
and are not contiguous with it.

Example footer shape:

```c
struct fffs_sector_footer {
    uint32_t serial;   // monotonic sector allocation/write serial
    uint8_t type;      // mixed data/metadata, directory, state, reserved
    uint8_t flags;     // valid/tombstone/reserved policy bits
    uint16_t reserved; // future CRC, footer size, or format flags
    uint32_t magic;    // sector-level FASTFFS magic, read first when parsing sector MD in reverse
};
```

The serial is 32-bit. It should advance when a sector is claimed for use after
erase, or equivalently at the first write that makes the sector FASTFFS-owned.
At one increment per claimed sector, 32 bits is unlikely to wrap before the
flash has exhausted its useful erase life for the target devices. If wrap ever
occurs, serials remain hints only; namespace correctness still comes from the
global index and metadata validation.

The serial gives two useful hints:

- the filesystem's relative allocation age
- each sector's relative last-allocation age for allocation and GC choices

A sector with a valid footer but no committed index path to it is
allocated/orphaned. It may still be used if there are free (erased) metadata and file data space. A sector
with an erased footer is unclaimed or fully erased. A partially programmed or
invalid footer should be treated as a failed claim/corrupt orphan unless a full
blank check proves the sector is erased. A footer that has been tombstoned means the entire sector is dead and can be erased.

Metadata records do not need their own magic field. 

## Sector Metadata

Sector metadata is out-of-line from the global index. It describes file data, extents, continuations, sizes, names, and local placement.

Metadata has multiple fixed-size record variants with different storage costs. Each record type has a known size. The type byte is placed at the physical end of the record so a reverse scanner can read the type first, derive the size, then read and validate the full record. The default implementation can start with a "does everything" record that supports file heads, extents, continuations, tombstones, size, and the configured filename limit. Later variants can specialize for long filenames, tiny files with short inline names, compact continuation records, or lightweight key/value records. Some variants may be compile-time configuration; others can be selected at runtime based on the file shape.

All metadata records include:

- type
- flags with valid/tombstone state
- resolved slot
- sector-local data offset/length for this extent
- optional CRC when required by the index header

The default file layout is linked single-extent metadata. Each metadata record describes one contiguous data extent. If a file continues into a non-contiguous sector, the current extent metadata links to the next extent's head sector.

The index-visible metadata record may represent either the beginning of a forward chain or the current tail of an append-optimized reverse chain. This does not change index replay or lookup; it only changes how open/read/write/seek enumerate extents.

Default root metadata should include:

- name length and filename, up to the default configured limit of 32 bytes
- total file size for fast `stat`
- next extent head sector, if any

Continuation metadata can be smaller:

- next extent head sector, if any
- no filename
- no total file size unless needed for validation

Continuations do not need the full root metadata fields.

Later metadata variants can add reverse tail records for cheap append. A tail/root record would carry the filename, total size, this extent, and links needed to find the previous and/or first extent. Runtime can read both forward and reverse-chain files; writing reverse-chain records can be a configuration choice.

Amendment records are also a later metadata variant. They are small slot-scoped records that update logical fields such as `next_extent_sector` or `total_size`; newest valid non-tombstoned amendment for a slot and field wins. Amendments do not change identity fields such as slot, filename, or data offset/length, and they do not mutate another record's lifecycle flags.

Continuation metadata still carries the resolved slot so GC can check liveness against the global index. A continuation from an overwritten file must not look live merely because the same slot was reused by a newer root.

Additional owner identity, such as root head, generation, or extent ordinal, is not part of the baseline. Liveness is validated by starting at the current index entry for the slot and walking the current root extent chain. A continuation with the same slot but not reachable from that chain is dead.

Metadata CRC support is optional, but if enabled it is a format-level policy advertised in index sector headers. A CRC-required image must not accept metadata records without valid CRC coverage as non-CRC records. CRC is not part of the 4-byte index record because that would destroy the compact index density.

## Local Tombstones

The global index delete is authoritative.

Sector-local tombstones are physical hints:

- help compaction/defrag
- allow metadata updates by writing a new metadata entry and tombstoning the old one
- are not required for namespace correctness

Tombstone state must be encoded as a monotonic NOR transition by clearing one bit in the metadata record. Tombstoned is persisted state; obsolete means a record has been superseded and is derived during replay or GC.

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

## Index-Only Cache Hash Table

The default replayed index cache can be a compact hash table that stores only
head sectors. It intentionally does not store resolved slots beside each head.
Slot identity is provided by the on-flash index record during replay and by
root metadata only when hash-cache collisions require disambiguation.

The compact hash table's worst-case probe work is bounded by the caller-provided
hash table size. There is no separate probe limit in the baseline; deployments
should size the table for their expected live-file count, RAM budget, and
acceptable collision rate.

Mount replay processes one index record at a time in journal order. It does not
look ahead through the index journal to skip obsolete records, and it does not
require obsolete metadata to remain valid. A put record can be inserted into
an empty hash-cache bucket as a head-only entry with no metadata read. Metadata
reads are only needed when the compact hash cache encounters an occupied
bucket/probe path and must decide whether the existing cached head or incoming
head belongs to the resolved slot being updated. If either side's metadata is
missing, tombstoned, corrupt, or resolves to a different slot, that side is
stale and may be evicted by the mutating replay/insert/remove path. Normal
read-side lookup does not repair or evict hash-cache entries; if a candidate
head cannot be verified, lookup skips that occupied bucket and continues
probing. A later index record for the same slot naturally overwrites or deletes
the earlier cached head as replay advances.

The index replay log can be older than the sectors they point to. It's possible for an index entry to point to an invalid head if it was subsequently deleted, and the sector erased and reused.

Instead of trying to read metadata on insert, we can check on delete. We'll need to check anyway to verify the delete is for the correct item.

During replay of a delete record, the remove code path must keep probing for the
deleted resolved slot until it finds a verified matching head or reaches the
hash-table probe bound. If a probed bucket's head reads as valid metadata for a
different resolved slot, the implementation must decide whether that different
slot can legally occupy that bucket under the current linear-probe cluster. If
not, the bucket is stale. The delete probe may discover multiple stale buckets.

Stale buckets can be marked with `head = 1` during the scan to remember that
they are invalid without creating empty-bucket gaps; `head = 1` is not a legal
data head because index sectors are reserved. After the scan finishes, clear the
stale markers and repair the affected linear-probe cluster once. This does not
prove that a stale bucket originally belonged to the deleted slot; it only
proves that the current bucket contents are not valid cache entries and must not
remain in the head-only table.

This keeps mount replay mostly index-only: put records can still be inserted
speculatively into empty buckets without reading metadata. Metadata reads are
paid by delete/remove, collision, and stale-cluster repair paths, not by every
index record.

This index hash table also limits the maximum number of files, since every file
must be stored in the hashtable. Performance degrades as the table fills.


## Configuration Options

Several parts of the design can be optional or compile-time/runtime configuration choices:

| Option | Purpose |
|---|---|
| cache full file metadata at startup | Faster `stat`, `ls`, and open; uses more RAM |
| cache only index at startup | Lower RAM; open may read one metadata page |
| no preload / lazy metadata reads | Lowest startup work; cold operations may probe flash |
| caller-provided static buffers | Keeps the embedded core usable without `malloc`/`free` |
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

GC should be incremental. The smallest useful GC step is one bounded physical
state transition or classification step:

- inspect/classify one sector-local metadata record and, if it is dead,
  optionally program that record's local tombstone bit
- or erase one sector that has already been classified as reclaimable

A GC step should not inspect metadata, tombstone it, and erase the sector in the
same call. Erase is large enough on target flash that it should be treated as a
whole step by itself. If metadata checking later becomes too expensive, the
metadata inspection step can be split further, but sector erase remains the
natural largest indivisible GC step.

The preferred scheduling knob is elapsed time, not a filesystem-wide "collect
everything" operation. User code can call a GC step whenever it has idle time,
measure its own wall-clock budget, and call again while idle time remains. A
future helper could loop over GC steps until a supplied time budget is exhausted,
but the core can remain usable with a simple one-step API and caller-managed
scheduling.

Sector-local compaction is TBD. It can behave like defrag: copy whole live files elsewhere, append normal overwrite records to the index, and allow the old sector to become reclaimable through ordinary GC. Because file size is known during compaction, it can try to choose contiguous sectors and reduce the number of extents. Flash does not materially care about sequential access, but contiguous placement benefits the linked-extent representation slightly by reducing metadata and seek traversal.

Wear leveling is intentionally simple in the baseline. Index rotation spreads index wear across the configured index sectors. The allocation cursor writes through unused/free sectors before wrapping, so with reasonable free space, data wear rotates through the partition. Static wear leveling by moving existing compact files is not planned for v1. If needed later, a non-file sector metadata record can store an erase counter; GC can update it after erase, and allocation can choose low-count sectors or a bounded low-count candidate near the cursor.

Local tombstones are hints for reclaim and compaction; the global index remains the authoritative namespace state.

## Corruption and Errors

Corruption is defined as invalid state that can cause a loss of previously successfully committed filesystem data/metadata. Partial writes due to power loss or crash must not cause corruption. Partial write crash recovery is normal/expected, and not considered corruption as long as only the partially written or uncommitted data/metadata is lost. Once an operation that commits data/metadata returns a non-error value, that data/metadata must not be lost/corrupted.

If corruption is detected at any point, a sticky corruption flag must be set.

If corruption is detected that impacts the operation, the operation should fail with an error and not complete normally.

In strict mode, any corruption detected must cause an operational failure, even if it does not directly impact the operation. In non-strict mode, corruption unrelated to the current operation only sets a sticky flag indicating some corruption has been detected, but may continue. A fsck/check could then be run later to find and possibly repair the corruption.

