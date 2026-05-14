# Existing Flash Filesystems Research

Research date: 2026-05-09

This note compares existing flash filesystems and flash object stores against the FASTFFS/NOR object filesystem sketch in `nor_object_fs_design.md`.

Target workload:

- Raw NOR flash, around 2-12 MB.
- Hundreds to low thousands of named files.
- Many tiny files, many files up to around 50 KB, a few around 300-350 KB.
- Hot operations: list names, open by name, read whole/partial files, create/overwrite whole files.
- Less important: arbitrary in-place mutation, full POSIX semantics, deep directory trees.

## Candidate Summary

Qualifying candidates must be open source and reasonably permissively licensed, or practically reusable without proprietary middleware.

| System | License/reuse | Type | Qualification | Notes |
|---|---|---|---|---|
| JesFS | MIT | NOR flash filesystem | Candidate | Closest practical open-source architectural competitor found so far. |
| SPIFFS | MIT | NOR flash filesystem | Baseline | Drop-in ESP-IDF baseline; known high-fill GC/write cliff. |
| LittleFS | BSD-3-Clause | COW/log filesystem | Baseline | Drop-in ESP-IDF baseline; robust, but many-file metadata traversal can dominate. |
| Zephyr NVS | Apache-2.0 | Flash key-value store | Research only | Relevant data+metadata sector layout; not a native file API and large values require chunking. |
| Zephyr ZMS | Apache-2.0 | Flash key-value store | Research only | NVS-like sector/ATE design; not a named filesystem and large files need chunking. |
| CFFS 2025 | Closed/research | Configured flash filesystem | Prior art only | Good paper result for configured/preallocated files, but not reusable as an implementation. |
| TFFS 2005 | Research | Research transactional NOR FS | Prior art only | Architecturally interesting, not a current implementation candidate. |
| YAFFS | GPL/commercial dual-license | Log flash filesystem | No | NAND/log heritage and licensing are a poor fit. |
| JFFS2 | GPL/Linux MTD | Log flash filesystem | No | Too OS-coupled and scan-heavy for this MCU/NOR target. |
| NuttX NXFFS | Apache-2.0 | Simple NOR FS | No | Useful prior art, but NuttX-coupled and sequential/repack behavior is not close enough. |
| NuttX SMARTFS | Apache-2.0 | Sector-mapped flash FS | No | Useful prior art, but more OS-coupled and POSIX-like than needed. |
| FileX + LevelX | MIT under Eclipse ThreadX | FAT-like FS over wear layer | No | FAT semantics plus translation layer are not a close architecture fit. |
| SEGGER emFile NOR | Commercial/proprietary | Commercial FS + NOR driver | No | Proprietary; prior art only. |

Current likely-near qualifying candidate count: **one, JesFS**.

## Architecture Notes

### JesFS

Local source: `third_party/JesFs`, commit `3a1dff76f7f5496e89792e5629673a52a0be89dc`.

JesFS targets serial NOR, small MCUs, low RAM, OTA/assets/config/logging, and power-loss persistence. Its API surface is small: `fs_start`, `fs_format`, `fs_open`, `fs_read`, `fs_write`, `fs_close`, `fs_delete`, `fs_rename`, `fs_info`, CRC/date helpers, and deep-sleep support.

Relevant traits:

- Sector-level allocation.
- `SF_SECTOR_PH` is fixed at 4 KiB in the tested source.
- Each file starts with a head sector; larger files chain continuation/data sectors.
- Tiny files effectively consume a whole 4 KiB head sector in this implementation.
- Sector 0 contains the filesystem header and a fixed array of 4-byte index entries pointing to file head sectors.
- With 4 KB sectors and 4-byte index entries, the index limit is about 1,000 files.
- File lookup is linear over used index entries: read index pointer, read head metadata, compare filename.
- Delete/overwrite marks old head/data sectors with monotonic sector-state transitions and can reuse deleted head slots.
- `fs_start` scans sector headers, so mount cost is proportional to partition size/sectors.

FASTFFS should beat JesFS on open/list once file counts grow because FASTFFS plans a compact append-only namespace journal plus RAM index, not linear index/head scans.

### SPIFFS

SPIFFS is relevant because it is NOR-oriented and object-based. It uses object IDs, object index pages, object lookup pages, and object index headers containing names/sizes.

Relevant traits:

- Flat namespace, no real directories.
- Tested ESP-IDF config uses `CONFIG_SPIFFS_PAGE_SIZE=256`.
- The effective tiny-file footprint is larger than one page because object/index/lookup metadata is also consumed.
- Page/object model can make warm opens much better than LittleFS in many-file cases.
- Garbage collection is synchronous and can require repeated scans.
- ESP-IDF reports usable SPIFFS bytes below raw partition bytes.

Observed limitation outside the clean benchmark snapshot:

- A baseline phase with 256 live 64-byte files consistently tripped SPIFFS before the medium-file phase.
- This is not raw byte capacity; 256 tiny files are only 16 KiB of payload.
- The failure points at SPIFFS object/page/metadata pressure near a 255/256 live-file edge under the current ESP-IDF configuration.
- The common benchmark now uses 192 tiny baseline files so the baseline stays under that edge after adding 16 medium files.

### LittleFS

LittleFS combines metadata-pair logs with COW data structures, especially CTZ skip-lists for file data. It is robust, open-source, and well-designed for bounded RAM. The concern for this project is many-file namespace cost: listing and opening can become metadata traversal dominated even when raw flash reads are cheap.

Tested ESP-IDF config uses `CONFIG_LITTLEFS_PAGE_SIZE=256`, `CONFIG_LITTLEFS_READ_SIZE=128`, `CONFIG_LITTLEFS_WRITE_SIZE=128`, `CONFIG_LITTLEFS_CACHE_SIZE=512`, and a 4 KiB erase/block size through the ESP flash partition. Tiny files are not forced to consume a whole 4 KiB sector, but they still pay metadata/log overhead.

### Zephyr NVS/ZMS

NVS and ZMS are close architectural references because they write data before metadata commit records and manage sectors as circular logs with a spare sector for GC. They are not native named filesystems. Large files require chunking, and chunking pushes file-level atomicity into a wrapper layer. They remain useful design references, not benchmark targets for this round.

### CFFS 2025

CFFS reports strong results for configured/preallocated files, including static, appendable, and circular files. It is closed/research code and does not satisfy the reusable open-source/permissive requirement. It remains useful prior art for configured-region performance.

## ESP32-S3 Benchmark Snapshot

Shared setup:

- Hardware: ESP32-S3 with 8 MB built-in GD flash.
- Filesystem partition: 4 MB data partition at `0x190000`.
- Baseline: 192 files of 64 B, then 16 files of 50 KiB.
- Churn target: about 8 MiB total logical writes, fixed 2,308,848 B live target, 128 KiB fill/free slack, seed `0x4f465346`.
- Churn distribution: mostly 10-20 KiB files, some 20-60 KiB files, and at least one 350 KiB write.
- Reads use permuted/randomized order, not sorted filename order.
- FASTFFS, JesFS, and LittleFS rows are from the newer erase-before-format harness. SPIFFS rows are from the previous fixed-live-target run and are slightly out of date; they are close enough for the high-level comparison, but should be refreshed when we are ready to spend the extra runtime.
- Delete-stat rows are pending a rerun after the harness correction that made replacement overwrite use the normal write path and made delete stats count only explicit churn deletes.

Raw partition timing is measured once for the ESP partition:

| Stat | Value |
|---|---:|
| Erase 4 KiB sector | 21.2 ms |
| Program 256-byte page, avg | 1.27 ms |
| Reprogram 256-byte page, avg | 1.36 ms |
| Read 4 B | 67 us |
| Read 256 B | 96 us |
| Read 4 KiB | 552 us |
| Erase 64 KiB range | 31.5 ms |
| Erase + program 4 KiB, 256 B pages | 46.7 ms |
| Erase + program 4 KiB, 1 KiB chunks | 38.4 ms |

Main comparison:

| Stat | FASTFFS default | JesFS | SPIFFS* | LittleFS |
|---|---:|---:|---:|---:|
| Reported usable capacity | 4,194,304 B | 4,194,304 B | 3,848,081 B | 4,194,304 B |
| Baseline preformat erase | 3.25 s | 3.77 s | old run | 2.04 s |
| Format time, housekeeping context | 42.0 ms | 2.97 s | 54.4 s | 57.1 ms |
| Mount after format | 496 us | 65.8 ms | 243 ms | 1.00 ms |
| Storage 192 x 64 B bytes/file | 4,138 B | 4,096 B | 502 B | 554 B |
| Storage 192 x 64 B overhead/file | 4,074 B | 4,032 B | 438 B | 490 B |
| Storage 208 mixed files bytes/file | 7,916 B | 7,876 B | 4,440 B | 4,608 B |
| Storage 208 mixed files overhead/file | 3,918 B | 3,879 B | 443 B | 610 B |
| Write 192 x 64 B total throughput | 17 KiB/s | 2 KiB/s | 0.2 KiB/s | 0.6 KiB/s |
| Read 192 x 64 B total throughput | 265 KiB/s | 4 KiB/s | 68.8 KiB/s | 3 KiB/s |
| Read 192 x 64 B open time/file | 81 us | 12.6 ms | 509 us | 9.61 ms |
| Read 192 x 64 B after-open throughput | 927 KiB/s | 306 KiB/s | 193 KiB/s | 6 KiB/s |
| Write 16 x 50 KiB total throughput | 180 KiB/s | 73 KiB/s | 65.8 KiB/s | 92 KiB/s |
| Read 16 x 50 KiB total throughput | 4,536 KiB/s | 1,237 KiB/s | 383 KiB/s | 2,384 KiB/s |
| Read 16 x 50 KiB open time/file | 87 us | 26.2 ms | 554 us | 2.92 ms |
| Read 16 x 50 KiB after-open throughput | 4,635 KiB/s | 3,523 KiB/s | 385 KiB/s | 2,805 KiB/s |
| Warm list, 208 files | 15.3 ms | 78.8 ms | 133 ms | 277 ms |
| Baseline exists, existing avg | 80 us | 12.2 ms | 51.1 ms | 17.6 ms |
| Baseline exists, missing avg | 507 us | 27.2 ms | 131 ms | 15.3 ms |
| Cold mount after baseline | 2.01 ms | 92.3 ms | 243 ms | 28.0 ms |
| Cold list, 208 files | 15.3 ms | 78.9 ms | 133 ms | 276.8 ms |
| Churn test total time | 106.9 s | 164.4 s | 826 s | 173.4 s |
| Churn logical bytes written | 8,396,458 B | 8,391,942 B | 8,391,942 B | 8,391,942 B |
| Churn final live bytes | 2,433,158 B | 2,434,361 B | 2,434,361 B | 2,434,361 B |
| Churn final live files | 124 | 123 | 123 | 123 |
| Churn final small files, 10-20 KiB | 116 | 116 | 116 | 116 |
| Churn final medium files, 20-60 KiB | 7 | 6 | 6 | 6 |
| Churn final large files, 350 KiB | 1 | 1 | 1 | 1 |
| Churn explicit deletes | 322 | 223 | 223 | 223 |
| Churn write 10-20 KiB total throughput | 182 KiB/s | 52 KiB/s | 10.6 KiB/s | 51 KiB/s |
| Churn write 20-60 KiB total throughput | 187 KiB/s | 57 KiB/s | 11.5 KiB/s | 60 KiB/s |
| Churn write 350 KiB total throughput | 188 KiB/s | 48 KiB/s | 6.5 KiB/s | 79 KiB/s |
| Churn delete p50 latency | pending rerun | pending rerun | 114 ms* | pending rerun |
| Churn delete p95 latency | pending rerun | pending rerun | 228 ms* | pending rerun |
| Churn delete max latency | pending rerun | pending rerun | 458 ms* | pending rerun |
| Churn final warm list | 9.31 ms | 74.4 ms | 126 ms | 399.8 ms |
| Churn final cold mount | 27.3 ms | 83.7 ms | 244 ms | 17.8 ms |
| Churn final cold list | 9.26 ms | 74.3 ms | 126 ms | 400 ms |
| Churn cold read 10-20 KiB total throughput | 4,381 KiB/s | 1,093 KiB/s | 245 KiB/s | 821 KiB/s |
| Churn cold read 10-20 KiB after-open throughput | 4,614 KiB/s | 3,486 KiB/s | 2,076 KiB/s | 1,364 KiB/s |
| Churn cold read 20-60 KiB total throughput | 4,515 KiB/s | 1,551 KiB/s | 323 KiB/s | 1,795 KiB/s |
| Churn cold read 20-60 KiB after-open throughput | 4,619 KiB/s | 3,503 KiB/s | 509 KiB/s | 2,305 KiB/s |
| Churn cold read 350 KiB total throughput | 4,559 KiB/s | 3,017 KiB/s | 453 KiB/s | 2,402 KiB/s |
| Churn cold read 350 KiB after-open throughput | 4,595 KiB/s | 3,525 KiB/s | 496 KiB/s | 2,506 KiB/s |
| Churn cold exists, existing avg | 77 us | 10.4 ms | 61.0 ms | 14.1 ms |
| Churn cold exists, missing avg | 268 us | 18.3 ms | 125 ms | 7.36 ms |

`*` SPIFFS values are from the older fixed-live-target snapshot. They predate explicit erase-before-format and the latest delete-stat harness, but they used the same 4 MB partition, same 60% live target, same logical churn size, same final live file count, and the same final size distribution.

## FASTFFS Variant Comparison

The default FASTFFS row is repeated here as the baseline for optional-feature comparisons. When a baseline benchmark feature changes, all FASTFFS variants in this table should be regenerated. When a specific optional FASTFFS feature is added or changed, only the affected variant set needs to be added or refreshed.

| Stat | Default: hash-heads + alloc map + debt GC | Hash-heads, no alloc map, on-demand GC | Hash-heads + alloc map, on-demand GC |
|---|---:|---:|---:|
| Index cache | hash heads | hash heads | hash heads |
| Allocation map | full bitmap | none | full bitmap |
| Benchmark-side GC | debt, max 16 steps/op | none | none |
| Baseline format | 42.0 ms | 42.4 ms | 46.5 ms |
| Baseline mount after format | 496 us | 271 us | 552 us |
| Write 192 x 64 B total throughput | 17 KiB/s | 18 KiB/s | 17 KiB/s |
| Read 192 x 64 B open time/file | 81 us | 83 us | 92 us |
| Write 16 x 50 KiB total throughput | 180 KiB/s | 186 KiB/s | 183 KiB/s |
| Read 16 x 50 KiB after-open throughput | 4,635 KiB/s | 4,637 KiB/s | 4,636 KiB/s |
| Warm list, 208 files | 15.3 ms | 15.3 ms | 15.3 ms |
| Churn logical bytes written | 8,396,458 B | 8,396,458 B | 8,396,458 B |
| Churn final live files | 124 | 124 | 124 |
| Churn write 10-20 KiB total throughput | 182 KiB/s | 18 KiB/s | 36 KiB/s |
| Churn write 20-60 KiB total throughput | 187 KiB/s | 19 KiB/s | 38 KiB/s |
| Churn write 350 KiB total throughput | 188 KiB/s | 22 KiB/s | 41 KiB/s |
| Churn benchmark-side GC time | 55.4 s | 0 s | 0 s |
| Churn GC erased sectors | 1,554 | inline only | inline only |
| Churn final cold mount | 27.3 ms | 27.0 ms | 27.2 ms |
| Churn final cold list | 9.26 ms | 9.26 ms | 9.24 ms |
| Churn cold read 350 KiB after-open throughput | 4,595 KiB/s | 4,597 KiB/s | 4,597 KiB/s |
| Churn cold exists, existing avg | 77 us | 78 us | 78 us |
| Churn cold exists, missing avg | 268 us | 269 us | 278 us |

The variant data shows that the allocation map helps the no-idle-GC worst case, but debt/idle GC is what restores foreground write throughput to the intended range. The read, list, mount, and existence results are mostly insensitive to these allocation/GC variants.

## Design Implications for FASTFFS

- Keep open/list/existence metadata lookup as a first-class performance target. The main competitor behavior to avoid is linear namespace/head scans at hundreds to low-thousands of files.
- Keep erase and reclaim out of foreground write latency where possible. SPIFFS-style synchronous GC can dominate write latency even when a filesystem reports substantial free space.
- Preserve file-level atomic replacement without requiring a chunking layer above an ID store.
- Raw flash reads are cheap enough that compact metadata probing is acceptable; erase and metadata topology dominate user-visible latency.
