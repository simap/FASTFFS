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

If no occupied slot exists in that range, the file cannot exist. If occupied
slots do exist in that range, the implementation reads the pointed root
metadata records and checks the filename. With a replayed in-RAM index cache,
missing lookups do not need to read every possible slot in the window, only
occupied candidate slots.

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

Index records do not carry per-record checksums. A record with all bits `1` is free space and marks the end of written records for that index sector. A record with all bits `0` is an invalid/clobbered record left by non-strict recovery of a terminal partial append, or invalid/clobbered state if it appears where recovery could not have produced it. Any other record is decoded and validated by context. File lookup/exists are a higher standard and would only show an existing file if its `head` points to a valid sector containing valid root metadata whose resolved slot matches the record. If that check fails for a committed record, corruption has occurred.

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

The `flags` byte uses mirrored lifecycle bits and policy bits:

- clear `0x80`: header is committed
- clear `0x40`: index sector is tombstoned
- clear `0x20`: metadata CRC is required

Committed and tombstoned are bit-level lifecycle states. Live means the
committed bit pair is cleared and the tombstone bit pair is still set.

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

## Startup & Mount

The phases of startup are:

1. Index sector discovery
2. Index log replay
3. Interrupted index maintenance recovery
4. Allocator/GC hint initialization
5. Optional background scanning

The later phases are covered in the index rotation, allocation, and reclaim
sections. This section focuses on index sector discovery and index replay.

### Index Log Replay

On startup:

1. Replay the index into RAM.
2. Later entries replace earlier entries.
3. Delete records remove earlier live entries.

Replay reads index records from the oldest selected sector through the active
sector. It stops at erased index-record space in each sector and remembers the
next append offset in the active sector. All-zero invalid/clobbered records are
skipped.
Live heads outside the data-sector range are rejected unless the record is the
terminal active-sector tail in non-strict mode.

The final record in the active index sector needs special handling because the
4-byte index append is the close commit point. A partial program can leave some
intended zero bits as ones, producing the wrong slot and/or head. Mount still
decodes and validates that record. If it is valid, replay accepts it. If it is
invalid, non-strict mount may recover it only when there are no later records to
replay: either it is physically the last record in the active index sector, or
the next record is erased and the remaining bytes after that erased record are
also erased. Replay should peek at the next record first; only if that record is
erased does it need to verify the rest of the sector. Recovery clobbers the
record to `00 00 00 00`, advances the next append offset past it, and stops
replaying the active sector. If any written record follows it, this is not the
terminal tail and mount reports corruption without modifying flash. Strict
mount also reports corruption for the invalid tail without modifying flash.

Index replay does not read file metadata. After replay reaches the end of the
selected index sequence, the in-memory index is the authoritative view of which
resolved slots exist. If a later operation follows an authoritative
`(slot, head)` mapping and finds missing, invalid, or wrong-slot root metadata,
corruption has occurred.

### Index Sector Discovery

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

Cold start cost is bounded by reading the compact index.

Startup caching is configurable:

- Low-RAM mode can scan the index and read root metadata only when an operation needs it.
- Default mode caches the replayed index in a hash table of
  `(slot, head)` entries, which makes missing lookups cheap without metadata
  reads.
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

An optional background blank check can prime in-memory allocation hints after
boot. If writing begins before this completes, allocation still blank-checks
candidates before use.

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

## Allocation Bitmap Hints

Allocation state can be tracked at FASTFFS sector level with an in-memory
bitmap. It is not persisted. On mount, the map starts from runtime-known state,
such as index sectors being unavailable, and allocation/GC refine it during
normal scans. The baseline can also be built with no allocation map, in which
case allocation and GC use linear scans plus blank checks and metadata
classification. A full bitmap uses caller-provided memory because it scales
with sector count; an 8 MB filesystem with 4 KB sectors needs 256 bytes.
Smaller coarse maps can be compile-time variants with inline storage.

The bitmap is an optimization, not the source of truth. On-flash sector
footers, metadata records, tombstones, inflight state, reservations, and blank
checks remain authoritative. A `0` bit means unknown or worth inspecting; it is
not proof of erased or usable space. For data sectors, a `1` bit normally means
the sector is known to contain live data and be full under current normal
allocation rules, so normal allocation and normal GC can skip it. "Full" uses
the allocator's practical rule: no useful allocation window remains, the hard
`FFFS_ALLOC_MAX_MD_RECORDS_PER_SECTOR` limit was reached, or continuation
metadata makes the sector ineligible for normal root sharing. Temporary
reservations may also set bits to mask
reserved sectors, and must clear them when the reservation is released, shrunk,
or consumed.

GC usually consults the bitmap before reading a sector. Deletes clear the old
file's known sector chain back to unknown, so GC can
later reclassify those sectors.
Allocation-pressure GC can run a second pass that ignores the bitmap.

A writer can set the bit when it knows the sector is full under the current
allocation rules, such as after writing continuation metadata or consuming the
remaining useful free window. GC can set the bit after it has classified live
metadata and proven the sector is full under normal allocation rules: it has a
live continuation, has reached the live-root record cap, or has no erased
window large enough for a new root record plus `FFFS_ALLOC_MIN_USABLE_FREE`.
The footer full bit by itself is not enough to update the bitmap, because the
sector may only contain obsolete or tombstoned data that GC should reclaim.

A full bitmap does require a decent chunk of memory. With 4K sectors, you'd need 256 bytes to cover an 8MB filesystem.

A coarse bitmap could compress this down significiantly, while still having some benefit and avoiding scanning some areas that are full for alloc or GC. For example, a 64 bit wide map could cover an 8MB filesystem using 32 sector wide buckets per bit.

Coarse map variants should use conservative "proven full bucket" semantics,
not "some live sector was full" semantics. Since allocation attempts to fill sequentially, there should be many contiguous, completely full blocks of sectors that can be skipped over. There would still be some partially full buckets, but it would still be less scanning that scanning every sector.

The allocator also keeps an `alloc_cursor`, the next sector to try for foreground allocation. Allocation is first-available from that cursor. New file roots and whole small files can share sectors with other roots. Continuations require an empty sector in normal allocation, so that compaction can do COW prefix compaction on a single sector. When allocation scans a sector and sees a continuation metadata record, it skips that sector for normal allocation and marks it full in the allocation bitmap. New writes fill a usable sector until it no longer has enough free space for the largest supported metadata record plus the configured minimum useful data space, or until a soft metadata-density target suggests moving on. The allocator does not try to hunt for sparse holes before filling the current usable sector.

The hard metadata-record cap provides an upper limit to file search time: allocation must not append past
it. A lower target density is only a post-allocation cursor policy, and a target
density of `0` advances after every allocation. Allocation
does not reject a sector merely because it is already at or above the target; if
the sector is otherwise usable and below the hard cap, allocation may append
one more record and then advance `alloc_cursor`. This lets small files spread
metadata lookup cost across more sectors while later alloc visits to those sectors still consumes
available space.

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
- Allocation bitmap updates are RAM-only hints. After a crash, mount starts with
  fresh hint state and flash remains authoritative.

A write can trigger allocation-pressure GC and root-only compaction before the
replacement index record is appended. If the old root was moved during that
allocation path and the original sector is reused for the new root, close must
refresh the current old head from the index before making the replacement
current and before tombstoning old metadata. The global index remains
authoritative; the refresh prevents stale per-open replacement state from
invalidating the wrong root.

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

Small files and file roots can share a sector. Larger files can spill into
continuation sectors.

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

Sector metadata is out-of-line from the global index. It describes file data, spans, continuations, sizes, names, and local placement.

Metadata uses fixed-size record variants with different storage costs. Each record type has a known size. The type byte is placed at the physical end of the record so a reverse scanner can read the type first, derive the size, then read and validate the full record. The baseline file variants are compact root and continuation records. Names and other variable-length root fields live in the root record's data-side prefix rather than inline in the metadata record.

File metadata records include:

- type
- flags with valid/tombstone state
- resolved slot
- next non-contiguous span head sector, if any
- contiguous span length
- sector-local data offset/length for this record's data-side region
- optional CRC when required by the index header

The default file layout is linked span metadata. Each file metadata record describes one contiguous physical span. If a file continues into a non-contiguous sector, the current span metadata links to the next span's head sector.

The index-visible metadata record may represent either the beginning of a forward chain or the current tail of an append-optimized reverse chain. This does not change index replay or lookup; it only changes how open/read/write/seek enumerate spans.

Root file metadata includes:

- total file size for fast `stat`
- root-sector data-side offset and length
- span linkage and span length

The root data-side region starts with the length-prefixed filename, followed by
any future length-prefixed root fields, then the root file bytes. File open by
name already has to parse the filename and can keep the computed root file byte
offset for later reads.

Continuation metadata includes:

- logical file offset for the continuation bytes
- continuation-sector data offset and length
- span linkage and span length
- no filename

Continuations do not need the full root metadata fields. Their data-side region
contains file bytes, not a required metadata prefix.

Later metadata variants can add reverse tail records for cheap append. A tail/root record would carry the filename, total size, this span, and links needed to find the previous and/or first span. Runtime can read both forward and reverse-chain files; writing reverse-chain records can be a configuration choice.

Amendment records are also a later metadata variant. They are small slot-scoped records that update logical fields such as `next_span_sector` or `total_size`; newest valid non-tombstoned amendment for a slot and field wins. Amendments do not change identity fields such as slot, filename, or data offset/length, and they do not mutate another record's lifecycle flags.

Continuation metadata still carries the resolved slot so GC can check liveness against the global index. A continuation from an overwritten file must not look live merely because the same slot was reused by a newer root.

Additional owner identity, such as root head, generation, or span ordinal, is not part of the baseline. Liveness is validated by starting at the current index entry for the slot and walking the current root span chain. A continuation with the same slot but not reachable from that chain is dead.

Metadata CRC support is optional, but it is a format-time runtime policy
advertised in index sector headers. Once mounted, the image policy determines
the physical metadata record size, validation rules, and allocation capacity
calculations for every file metadata record in that image.

CRC does not define a separate root or continuation metadata type. The terminal
type byte still identifies the logical metadata shape. When metadata CRC is
required, the physical record form is:

```text
[crc32][lifecycle][body...][type]
```

A reverse scanner reads the terminal type first, derives the base record size
from the type, then adds the CRC prefix size required by the mounted image
policy.

A CRC-required image must not accept the non-CRC physical length of a record as
valid metadata. A non-CRC image must not reinterpret a CRC-prefixed physical
record as current metadata. Mixed CRC/non-CRC metadata within one mounted image
is not a baseline format.

Metadata CRC32 excludes the stored CRC field and excludes the lifecycle byte.
The baseline writer may program the lifecycle byte in the same record write as
the CRC-covered body; lifecycle-last commit remains a possible later hardening
choice if partial-write behavior warrants it. CRC covers the record's claimed
sector-local data bytes and the immutable metadata body through the terminal
type byte. CRC is not part of the 4-byte index record because that would destroy
compact index density.

## Local Tombstones

The global index delete is authoritative.

Sector-local tombstones are physical hints:

- help compaction
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
blank check + program data + local metadata write + one index append
```

Using measured approximate numbers:

- read/blank-check 256 bytes: ~100 us
- program 256 bytes: ~670 us
- tiny index write: <= ~380 us
- optional sector full-hint program: <= ~380 us

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

### Allocation Strategy

Allocation should consider the following:
* Allocate sectors with enough free space for metadata plus a minimum amount of data. In other words, it should try to use leftover free space in a sector.
* Multiple open writable files should not end up being allocated the same sectors. This ensures that open files can write contiguously to the data area without conflict.
* It should not allocate sectors being used for any open files.
* Continuation allocations should only use empty sectors.
* Normal root allocations should skip sectors containing continuation metadata, but may share sectors containing other roots.
* Ideally contiguous sectors are allocated when available. 
* Sectors must be verified to have erased usable space before being allocated. Allocation must verify flash contents, not just metadata or allocation hints.

A reasonable starting minimum data threshold is 128-256 bytes. The exact formula
is a tunable definition, but runtime allocation should be a simple range check.

Allocator policy should also reserve some metadata slack for later tombstones
and amendment records. The exact reserve is TBD. It might be configurable, with
reserve = 0 effectively disabling MD amendment records.

The allocator may temporarily reserve some sectors in a contiguous extent for a file, if those sectors appear to be potentially free, and later allocate them in sequence when requested. These are temporarily unavailable for allocation/reservation to other files. The allocator may change the reservations for any open file as needed, for example under space pressure. The reservation size should be limited and configurable.

Reservations don't need to be blank checked or verified until allocated.

Reservation state can be represented as temporary unavailable bits in the
allocation bitmap. These bits must be cleared when a reservation is released,
shrunk, consumed, or fails allocation. If allocation cannot find space while
respecting the bitmap, it can cut reservations and retry. If cutting
reservations does not release any sectors, all file reservations were already
zero.

The allocator can avoid allocating sectors potentially used by other files by
using the open/in flight file data to see what sectors are not fully written,
and by skipping over sectors that are reserved for or currently used by open
writers.

Under reservation pressure, the allocator should cut reservations for open files by half, rounded down (right shift by 1). This either freed up one or more reserved sectors, or all reservations were already zero. This would hopefully free up smaller but still contiguous ranges. Every file gets equal 50% cut. If a file was cut and then later wrote and alloced, it would attempt to extend/reclaim the contiguous reservation if available. Less active writers would have smaller and smaller reservations, while active writers would reclaim reservations more aggressively.

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
| write 50 KB to blank pages | `50 KB / 256 * 670 us`, about ~134 ms plus metadata/index/full-hint writes |
| write 300 KB to blank pages | `300 KB / 256 * 670 us`, about ~804 ms plus metadata/index/full-hint writes |
| program 4 KB blank sector | ~9.1 ms |
| erase + program 4 KB sector | ~53 ms |
| append one small index record | <= ~380 us |
| program sector full hint | <= ~380 us |

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
- candidate slots skipped by the RAM index cache

Duplicate raw hashes are not the baseline because they change delete and replay semantics: the index key is no longer unique without consulting metadata.

## Replayed Index Cache

The replayed index cache is an in-memory accelerator for the authoritative
append-only index journal. It is not a second source of truth. Mount replay
applies index records in journal order:

- a put record stores or replaces the current `slot -> head` mapping,
- a delete record removes the current mapping for that slot,
- later records win over earlier records,
- replay does not read file metadata to prove that an index record is valid.

After replay reaches the end of the selected index sequence, the in-memory
cache represents the authoritative view of which resolved slots exist. All-zero
invalid/clobbered records are recovery markers and do not affect the namespace.
If a later operation reads root metadata through an authoritative `(slot, head)`
mapping and the metadata is missing, invalid, or belongs to a different slot,
corruption has occurred.

### Hash Table Cache

The default replayed index cache is an open-addressed hash table of
`(slot, head)` entries. The resolved slot is the hash-table key and determines
the entry's home bucket:

```text
home = slot & (bucket_count - 1)
```

`slot == 0` is reserved as the empty-bucket marker, matching the resolved-slot
rules that avoid `0x0000`. The table size is a caller-provided power of two.
There is no separate cache-level probe limit; deployments should size the table
for their expected live-file count, RAM budget, and acceptable collision rate.
Every live file needs one hash-cache bucket, so performance degrades as the
table fills, but remains an in-memory search.

Hash-cache insertion and deletion are purely RAM/index operations:

- insert probes by stored slot and writes `(slot, head)` into an empty or
  matching bucket,
- overwrite replaces the head for the same slot,
- delete removes the matching slot entry,
- cluster repair moves entries according to their stored slots and does not
  consult metadata.

Because the slot key is stored in RAM, hash-cache replay, bucket insertion,
bucket deletion, lookup, and repair do not need root metadata reads. This gives
the hash table cache the same flash behavior as a full slot-head table for
slot lookup paths, while using `4 * bucket_count` bytes instead of a fixed
128 KiB full-slot table.

The cache only answers slot-to-head questions. Filename-to-slot probing is the
resolved-slot namespace rule described in the global index section. During
lookup by name, the cache is used to test whether each candidate resolved slot
is occupied and, when occupied, to return the head sector for that slot. Name
comparison happens only after reading the root metadata for that exact
`(slot, head)`.

This creates two layered hash tables with different meanings. The global
namespace hash/probe rule assigns a sticky resolved slot to a file and persists
that slot in the index log. The in-memory index-cache hash table only stores
current `(slot, head)` mappings for fast lookup. Cache buckets may move during
collision handling or cluster repair, but the resolved slot key does not change.

Example:

```text
Global index (persistent) resolved slot allocation            
                                                              
  hash("cfg/net") -> base slot abcd                           
                         │                                    
                         ▼                                    
        ◄───────────probe window─────►                        
        abcd    abce      abcf  ...                           
         ▼       ▼         ▼                                  
    occupied  occupied   free                                 
                           │                                  
                           ▼                                  
                    index log stores:                         
                    (abcf -> head 42)                         
                                                              
RAM index cache:                                              
  bucket count = 8                                            
  home bucket = abcf & 7                                      
                                                              
              home bucket occupied by another cache entry     
              │                                               
              │      abcf,42 was bumped to the adjacent bucket
              │      │                                        
     bucket   ▼      ▼                                        
       0      1      2      3      4      5      6      7     
     +------+------+------+------+------+------+------+------+
slot:|      | b015 | abcf |      |      |      |      |      |
head:|      |  17  |  42  |      |      |      |      |      |
     +------+------+------+------+------+------+------+------+
```

Directory listing iterates cache entries and reads the root metadata
for each entry's exact `(slot, head)`. It must not treat a head sector as having
one canonical file identity, because one sector may contain root metadata for
multiple files.

### Full Slot-Head Cache

The full slot-head cache stores one 16-bit head value for every resolved slot.
It uses 128 KiB with the v1 16-bit slot namespace. Slot lookup is direct:

```text
head = heads[slot]
```

This mode avoids hash-cache collision behavior entirely and can be more
RAM-efficient than the hash table cache for very large live-file counts:

```text
hash cache bytes = 4 * bucket_count
full slot cache bytes = 2 * 65536 = 128 KiB
```

The hash table cache is more RAM-efficient below 32K buckets. At 32K or more
entries, the full slot-head table uses the same or less RAM and provides direct
slot addressing without conflicts. Both modes are otherwise equivalent at the
flash-operation level: root metadata is still read only when a caller needs
names, stats, root file data, or consistency validation.

### No-Cache Mode

The lowest-RAM mode keeps no replayed namespace cache. Slot lookup, filename
resolution, directory iteration, GC liveness, and index compaction scan the
on-flash index as needed. It minimizes RAM, but may read one or more index 
sectors from flash before they can identify the current head for a slot (or
prove that it doesn't exist).

### Head-Only Hash Table Cache Concept (Tabled)

A smaller hash table cache can store only head sectors and recover the slot key
from root metadata when collisions or deletes need disambiguation. This saves
two bytes per bucket compared to `(slot, head)`, but it makes bucket mechanics
depend on sector-local metadata.

That design is problematic once a head sector can contain root metadata for
multiple files. The head sector is no longer a unique file key, so the cache
cannot reliably answer "which slot does this bucket represent?" by reading a
single sector-level metadata identity. It also risks treating metadata
disagreement as a cache-repair signal even though the global index is the
authoritative existence state. For multi-metadata sectors, the baseline hash
table cache stores the slot key explicitly.

This may still be useful if small file storage compactness was less of a 
concern. If configured so that no sector is shared, a head-only hash table
can have twice as many buckets as the (slot,head) hash table for the same RAM.

This still needs to read file metadata in various circumstances, but is much
faster than no-cache full index scans.

## Configuration Options

Several parts of the design are compile-time policy choices, runtime mount
choices, or format-time image choices. Current implemented knobs:

| Option | Kind | Purpose |
|---|---|---|
| `sector_size` | format-time | Allocation/index/reclaim unit; encoded as `256 << sector_shift`, default 4 KB |
| `index_sectors` | format-time | Number of rotating index sectors; must be at least 2 |
| backend geometry | runtime | Backend size and read/program/erase granules; FASTFFS sector size must fit backend constraints |
| `strict` mount | runtime | Controls whether narrow non-strict recovery exceptions are allowed |
| caller-provided scratch | runtime | Required working buffer; keeps the embedded core usable without `malloc`/`free` |
| `FFFS_INDEX_CACHE_MODE` | compile-time | Selects no-cache, hash `(slot, head)` cache, or full slot-head cache |
| `index_hash_table_size` | runtime | Cache entry count for the selected index mode; hash mode requires a bounded power-of-two count, full slot-head mode requires `FFFS_SLOT_COUNT` |
| `FFFS_INDEX_HASH_TABLE_SIZE` / `FFFS_INDEX_HASH_TABLE_SIZE_MAX` | compile-time | Default and upper bound for hash-cache sizing |
| `FFFS_ALLOC_MAP_MODE` | compile-time | Selects no allocation map or full in-memory bitmap hints |
| `alloc_map` / `alloc_map_words` | runtime | Caller-provided RAM storage for the full bitmap when compiled in |
| `FFFS_ALLOC_RECOVERY_LOOKAHEAD` | compile-time | Bounds allocator cursor recovery from recent index heads |
| `FFFS_ALLOC_MIN_USABLE_FREE` | compile-time | Minimum useful data window before a sector is treated as full for normal allocation |
| `FFFS_ALLOC_TARGET_DENSITY` | compile-time | Soft cursor-advance target after allocation; not a hard capacity limit |
| `FFFS_ALLOC_MAX_MD_RECORDS_PER_SECTOR` | compile-time | Hard metadata-record cap for allocation/search cost |
| `FFFS_ALLOC_RESERVE_SECTORS` | compile-time | Per-file reservation count for contiguous continuation allocation |
| `FFFS_COMPACTION_RESERVE_SECTORS` | compile-time | Root-compaction destination reserve count |
| `FFFS_COMPACTION_RESERVE_BEST_FIT` | compile-time | Optional best-fit selection inside the compaction reserve bank |
| `FFFS_COMPACTION_CANDIDATE_COUNT` | compile-time | Number of root-only compaction candidates retained during pressure GC |
| `FFFS_GC_ON_ALLOC_FAILURE` | compile-time | Enables foreground GC/compaction when allocation cannot find space |
| `FFFS_GC_PARANOID_REACHABILITY` | compile-time | Debug/paranoid reachability check during GC classification |
| `FFFS_FILE_CACHE_SIZE` | compile-time | Per-file IO cache and flash program chunk size; must satisfy backend program granule needs |
| `FFFS_MD_PRELOAD_MAX` / `FFFS_MIN_SCRATCH_SIZE` | compile-time | Bounds stack/caller scratch used for metadata and flash reads |
| `FFFS_INDEX_COMPACT_OUTER_SCAN_SIZE` / `FFFS_INDEX_COMPACT_WRITE_BUFFER_SIZE` | compile-time | Bounds index compaction scan/write buffering |
| `FFFS_LAZY_DELETE_TOMBSTONES` | compile-time | Defers delete-time local tombstone programming; global index remains authoritative |
| `FFFS_PROFILE_TRACE` | compile-time/runtime | Enables optional flash/profile trace callback |

Design-only or future knobs:

| Option | Status |
|---|---|
| cache full file metadata at startup | Future larger-MCU mode |
| cache extent list on open or first seek | Future per-open seek acceleration |
| background blank-check scan on boot | Future map warmup; current allocation verifies candidates as needed |
| metadata CRC32 | Planned format-time policy, not current baseline |
| directory objects | Optional future secondary index |
| metadata amendment records | Future metadata variant |
| background erase scheduling helper | Future helper; current API exposes incremental GC steps |
| wear-level erase counters | Future wear-leveling input, not part of allocation/compaction scoring today |

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

Data-sector compaction is currently root-only. This is intentionally narrower
than general compaction: it does not copy continuation data, does not rewrite
whole files, and does not need multi-sector space analysis. Normal allocation
keeps continuations isolated so GC can identify root-only sectors with a local
metadata walk.

During the full allocation-pressure `fffs_gc_until_erased` scan, GC keeps a
small sorted top-`n` list of compaction candidates. A candidate is a sector with
live root metadata, no live continuation metadata, and no inflight writer state.
The recorded candidate facts are:

- sector number
- trapped reclaimable bytes
- live root count

Candidate ranking keeps sectors with useful trapped reclaimable bytes ahead of
sub-threshold candidates. Within the useful tier, it prefers more trapped
reclaimable bytes, then more live roots. Within the sub-threshold tier, it
prefers more live roots, then more trapped reclaimable bytes. This keeps the
ranking tied to actual space/copy shape instead of mixing unrelated weights
such as wear count into the reclaim decision.

If erase-only GC cannot free a sector and fsinfo estimates enough unused space
to try a root-only move, GC copies live roots out of the best candidate. Each
copied root is written as ordinary root metadata at a valid destination window,
and an index record is appended so the copied root becomes current. The
destination allocator skips the source sector and inflight sectors. It uses a
relaxed root-placement policy, so copied roots may land in sectors that contain
continuation metadata under pressure, while still requiring erased writable
space and rejecting ordinary allocation conflicts. If all live roots are copied,
the source sector is erased. If the copy fails partway through, old roots that
were successfully copied are tombstoned only after reclassification shows they
are no longer current.

The current implementation intentionally leaves continuation-held reclaimable
space alone. Compacting mixed continuation/root sectors, continuation-only tail
waste, linked span prefixes, and whole files requires a separate general
compaction design that accounts for copied bytes, destination space, source
sectors made freeable, new trapped space, erase cost, wear, and foreground
stall time.

Wear leveling is intentionally simple in the baseline. Index rotation spreads index wear across the configured index sectors. The allocation cursor writes through unused/free sectors before wrapping, so with reasonable free space, data wear rotates through the partition. Static wear leveling by moving existing compact files is not planned for v1. If needed later, a non-file sector metadata record can store an erase counter; GC can update it after erase, and allocation can choose low-count sectors or a bounded low-count candidate near the cursor.

Local tombstones are hints for reclaim and compaction; the global index remains the authoritative namespace state.

### Future Reclaim Scheduling: Clean Scan Counter, Delete Queue, Erased Bitmap

A smart GC mode is desirable in FASTFFS. During quiescent periods, background GC should settle to no-ops, not 0.6 ms sector scans forever. GC should be smart about where it spends its time.

#### Garbage Sources

- Garbage is created by delete, overwrite, failed/dangling writes, and partial-write power loss. The first two are covered in normal operation.
- If a file never closes, it's not reclaimable anyway. A failed close gets cleaned on next boot with the other power-loss scenarios.

#### GC Clean Scan Counter

Keep a GC `clean_scan` counter, reset on normal-operation garbage-creating events left untracked by the delete queue (delete, overwrite, or a future close-abandon). Incremented **after** GC completes scanning a sector (not each step). When `clean_scan >= data_sectors`, GC does not need to scan sectors anymore, and can no-op and return idle.

This is a cheap option that solves the biggest issue with background gc steps on a quiescent system: burning time/cycles re-checking the same sectors over and over. On boot it still scans each sector once to clean up any previous leftover garbage.

#### GC Delete Queue

Add a configurable size unique queue of `n` tombstoned file heads. GC steps work on these first over starting new sector scans. It's a new type of scan that walks link/span info of tombstoned files with heads in the queued sector.

- Queued item is popped and turned into gc step state. If the same sector is queued again while scanning, it is queued like normal and gets another scan later.
- Don't live-reachability check every file in the walked sectors, just use the tombstone bits. Regular sector scans catch orphaned non-tombstoned data.
- Still read what is in each sector before erasing it, and check tombstone bits. Stop walking for that file if the slot is not tombstoned or missing (e.g. reused already).
- A pointer to a sector could have multiple tombstoned heads; GC has to scan each, since a queued sector alone is ambiguous.
- Previously deleted mid/tail sectors dead-end this walk early, when GC doesn't find that tombstoned slot in the sector anymore.
- The queue pushes out old entries when it overflows, which must clear the clean_scan counter.
- Alloc-pressure-triggered GC can use the recent delete queue for fast pressure relief, then fall back to sector-by-sector sweeps.
- Erase from the file tail, so that the link/span information isn't severed.
- Since this has no live-reachability check and the walk is not very expensive, a single step in this mode could walk multiple sectors for a file, possibly to the tail each step. Keep tail reclaim to its own step. A 350K file walk is about 6.2ms.
- After all tombstoned files are reduced to heads, the sector itself is considered for reclaim per normal GC rules.

#### Known Erased+Free Bitmap — optional

Record a known erased + free bitmap, shared and usable between alloc and GC.

Improves the GC sector scan in degraded cases when above optimizations fail/overload or are disabled, and alloc in normal cases. Churn/thrash workloads would benefit from the bitmap. GC effectively hands off sectors to alloc.

## Corruption and Errors

Corruption is defined as invalid state that can cause a loss of previously successfully committed filesystem data/metadata. Partial writes due to power loss or crash must not cause corruption. Partial write crash recovery is normal/expected, and not considered corruption as long as only the partially written or uncommitted data/metadata is lost. Once an operation that commits data/metadata returns a non-error value, that data/metadata must not be lost/corrupted.

If corruption is detected at any point, a sticky corruption flag must be set.

If corruption is detected that impacts the operation, the operation should fail with an error and not complete normally.

In strict mode, any corruption detected must cause an operational failure, even if it does not directly impact the operation. In non-strict mode, corruption unrelated to the current operation only sets a sticky flag indicating some corruption has been detected, but may continue. A fsck/check could then be run later to find and possibly repair the corruption.

The active index tail exception is deliberately narrow. Non-strict mount may
repair one invalid terminal tail record by clobbering it to all zeros only when
no later index records exist. If strict mode is enabled, or if any later written
record exists, mount must fail without programming the index. Clobbering treats
the interrupted close as uncommitted and makes the index appendable again. The
exception is a gray area without index CRC32: it can hide a genuinely committed
terminal entry whose slot/head bits were later lost. Strict mode rejects it, and
CRC32-backed index entries should replace the heuristic with positive
partial-entry detection. A fsck tool may be able to recover some non-strict
cases by searching for orphaned roots whose slot/head bits are compatible with
the partially programmed entry.
