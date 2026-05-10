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

## Fresh ESP32-S3 Benchmark Snapshot

Status: fixed-live-target comparison snapshot captured for JesFS, SPIFFS, and LittleFS.

Shared setup:

- Hardware: ESP32-S3 with 8 MB built-in GD flash.
- Filesystem partition: 4 MB data partition at `0x190000`.
- API path: filesystem API through the closest available file operations.
- Baseline: 192 tiny files of 64 B, then 16 medium files of 50 KiB.
- Churn target: about 8 MiB total logical writes.
- Churn live target: fixed at 2,308,848 B for all filesystems, matching 60% of SPIFFS reported usable capacity.
- Churn fill/free slack: 128 KiB.
- Churn seed: `0x4f465346`.
- Churn distribution: mostly 10-20 KiB files, some 20-60 KiB files, and a forced 350 KiB write after 7 MiB total written if no large write has occurred yet.
- Read order: permuted/randomized, not sorted by filename.
- Raw partition timing is measured once, not once per filesystem.
- Transcripts:
  - `benchmarks/results/jesfs_fixed_20260509_210338.log`
  - `benchmarks/results/spiffs_fixed_20260509_210338.log`
  - `benchmarks/results/littlefs_fixed_20260509_210338.log`
- Storage overhead transcripts:
  - `benchmarks/results/jesfs_overhead_20260510.log`
  - `benchmarks/results/spiffs_overhead_20260510.log`
  - `benchmarks/results/littlefs_overhead_20260510.log`

Raw partition timing:

The 256-byte page program and reprogram rows are 16-sample min/average/max measurements over a freshly erased 4 KiB sector. These replace the earlier single-sample page-program result, which was an outlier.

| Stat | Value |
|---|---:|
| Erase 4 KiB sector | 21.269 ms |
| Program 256-byte page, min | 730 us |
| Program 256-byte page, avg | 1.043 ms |
| Program 256-byte page, max | 5.552 ms |
| Reprogram 256-byte page, min | 703 us |
| Reprogram 256-byte page, avg | 1.338 ms |
| Reprogram 256-byte page, max | 10.612 ms |
| Read 4 B | 65 us |
| Read 256 B | 89 us |
| Read 4 KiB | 554 us |
| Erase 64 KiB range | 33.859 ms |
| Erase + program 4 KiB, 256 B pages | 45.587 ms |
| Erase + program 4 KiB, 1 KiB chunks | 38.036 ms |

Comparison table:

| Stat | JesFS | SPIFFS | LittleFS |
|---|---:|---:|---:|
| Reported usable capacity | 4,194,304 B | 3,848,081 B | 4,194,304 B |
| Format time, housekeeping context | 26.3 s | 54.4 s | 84.6 ms |
| Mount after format | 65.9 ms | 243.0 ms | 1.0 ms |
| Storage base used after format | 4,096 B | 0 B | 8,192 B |
| Storage 192 x 64 B used delta | 786,432 B | 96,384 B | 106,496 B |
| Storage 192 x 64 B bytes/file | 4,096 B | 502 B | 554 B |
| Storage 192 x 64 B overhead/file | 4,032 B | 438 B | 490 B |
| Storage 192 x 64 B overhead | 6300% | 684% | 766% |
| Storage 208 mixed files used delta | 1,638,400 B | 923,680 B | 958,464 B |
| Storage 208 mixed files bytes/file | 7,876 B | 4,440 B | 4,608 B |
| Storage 208 mixed files overhead/file | 3,879 B | 443 B | 610 B |
| Storage 208 mixed files overhead | 97% | 11% | 15% |
| Write 192 x 64 B total throughput | 2.9 KiB/s | 0.2 KiB/s | 0.6 KiB/s |
| Read 192 x 64 B total throughput | 4.9 KiB/s | 68.8 KiB/s | 3.3 KiB/s |
| Read 192 x 64 B open time/file | 12.6 ms | 509 us | 9.6 ms |
| Read 192 x 64 B after-open throughput | 306.0 KiB/s | 193.0 KiB/s | 6.0 KiB/s |
| Write 16 x 50 KiB total throughput | 74.7 KiB/s | 65.8 KiB/s | 72.2 KiB/s |
| Read 16 x 50 KiB total throughput | 1238.0 KiB/s | 383.3 KiB/s | 2401.5 KiB/s |
| Read 16 x 50 KiB open time/file | 26.2 ms | 554 us | 2.9 ms |
| Read 16 x 50 KiB after-open throughput | 3523.0 KiB/s | 385.0 KiB/s | 2814.0 KiB/s |
| Warm list, 208 files | 78.9 ms | 133.5 ms | 276.6 ms |
| Baseline exists, existing file avg | 12.2 ms | 51.1 ms | 17.6 ms |
| Baseline exists, missing file avg | 27.2 ms | 131.5 ms | 15.3 ms |
| Cold mount after baseline | 92.4 ms | 243.3 ms | 28.1 ms |
| Cold list, 208 files | 78.9 ms | 133.4 ms | 276.4 ms |
| Cold read 32 x 64 B open time/file | 12.5 ms | 46.0 ms | 10.3 ms |
| Cold read 4 x 50 KiB open time/file | 26.1 ms | 36.0 ms | 3.3 ms |
| Churn test total time | 162.8 s | 826.1 s | 182.9 s |
| Churn logical bytes written | 8,391,942 B | 8,391,942 B | 8,391,942 B |
| Churn final live bytes | 2,434,361 B | 2,434,361 B | 2,434,361 B |
| Churn final live files | 123 | 123 | 123 |
| Churn final small files, 10-20 KiB | 116 | 116 | 116 |
| Churn final medium files, 20-60 KiB | 6 | 6 | 6 |
| Churn final large files, 350 KiB | 1 | 1 | 1 |
| Churn creates | 346 | 346 | 346 |
| Churn replacements | 114 | 114 | 114 |
| Churn deletes | 223 | 223 | 223 |
| Churn write 10-20 KiB total throughput | 53.1 KiB/s | 10.6 KiB/s | 48.3 KiB/s |
| Churn write 20-60 KiB total throughput | 57.9 KiB/s | 11.5 KiB/s | 56.3 KiB/s |
| Churn write 350 KiB total throughput | 49.3 KiB/s | 6.5 KiB/s | 87.3 KiB/s |
| Churn write p50 latency | 271.8 ms | 1.15 s | 241.1 ms |
| Churn write p95 latency | 674.5 ms | 5.86 s | 919.8 ms |
| Churn write max latency | 7.10 s | 54.2 s | 4.01 s |
| Churn delete p50 latency | 23.5 ms | 113.6 ms | 20.3 ms |
| Churn delete p95 latency | 32.2 ms | 227.6 ms | 22.8 ms |
| Churn delete max latency | 43.3 ms | 458.4 ms | 725.3 ms |
| Churn final warm list, 123 files | 74.4 ms | 125.8 ms | 399.7 ms |
| Churn final cold mount | 83.7 ms | 243.7 ms | 17.8 ms |
| Churn final cold list, 123 files | 74.3 ms | 125.6 ms | 399.6 ms |
| Churn cold read 12 x 10-20 KiB total throughput | 1092.7 KiB/s | 245.3 KiB/s | 824.5 KiB/s |
| Churn cold read 12 x 10-20 KiB open time/file | 9.7 ms | 55.3 ms | 7.3 ms |
| Churn cold read 12 x 10-20 KiB after-open throughput | 3486.0 KiB/s | 2076.0 KiB/s | 1375.0 KiB/s |
| Churn cold read 6 x 20-60 KiB total throughput | 1552.0 KiB/s | 322.6 KiB/s | 1792.2 KiB/s |
| Churn cold read 6 x 20-60 KiB open time/file | 15.3 ms | 48.6 ms | 5.2 ms |
| Churn cold read 6 x 20-60 KiB after-open throughput | 3503.0 KiB/s | 509.0 KiB/s | 2318.0 KiB/s |
| Churn cold read 1 x 350 KiB total throughput | 3017.1 KiB/s | 453.2 KiB/s | 2405.3 KiB/s |
| Churn cold read 1 x 350 KiB open time/file | 16.7 ms | 67.9 ms | 5.8 ms |
| Churn cold read 1 x 350 KiB after-open throughput | 3525.0 KiB/s | 496.0 KiB/s | 2509.0 KiB/s |
| Churn cold exists, existing file avg | 10.4 ms | 61.0 ms | 14.2 ms |
| Churn cold exists, missing file avg | 18.3 ms | 125.0 ms | 7.4 ms |

Notes on the snapshot:

- The fixed-live-target run makes the churn stream identical across all three filesystems: 460 writes, 223 deletes, 123 final live files, and the same final size distribution.
- SPIFFS completed the run with no short writes, zero-write retries, flush failures, or close failures. It still hit the expected write cliff: p95 write latency was 5.86 s and the forced 350 KiB write took 54.2 s.
- JesFS and LittleFS both kept write throughput near the raw erase/program envelope for this workload, but they spend time in different places. JesFS has slower open/existence for larger baseline files because name lookup walks index/head metadata. LittleFS has very fast mount and medium-file reads, but list and tiny-file reads are metadata-heavy.
- Cold read rows include open time. The after-open rows better represent streaming read speed once the file is already open.
- Write timings still measure total create/replace time, including open/write/flush/close. A future harness revision should split write-open from after-open write throughput to match the read terminology.
- Format time is kept as housekeeping context, not as a primary user operation. It can indicate whether a filesystem pays erase/free-map cost upfront, in the background, or on-demand during later writes.

## Design Implications for FASTFFS

- Keep open/list/existence metadata lookup as a first-class performance target. The main competitor behavior to avoid is linear namespace/head scans at hundreds to low-thousands of files.
- Keep erase and reclaim out of foreground write latency where possible. SPIFFS-style synchronous GC can dominate write latency even when a filesystem reports substantial free space.
- Preserve file-level atomic replacement without requiring a chunking layer above an ID store.
- Raw flash reads are cheap enough that compact metadata probing is acceptable; erase and metadata topology dominate user-visible latency.
