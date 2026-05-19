# Existing Flash Filesystems Research

Research date: 2026-05-09

This note compares existing flash filesystems and flash object stores against the FASTFFS design in `design.md`.

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

### Benchmark Methodology

- Hardware: ESP32-S3 with 8 MB built-in GD flash.
- Filesystem partition: 4 MB data partition at `0x190000`.
- Baseline: 192 files of 64 B, then 16 files of 50 KiB.
- Reads use permuted/randomized order, not sorted filename order.

The current benchmark runner is `benchfs`, a shared benchmark flow with filesystem-specific adapters. The common runner owns workload generation, timing, accounting, churn modeling, stats, and table-log output. Adapters only provide format/mount/unmount, file operations, listing/existence checks, optional filesystem info, optional GC stepping, and optional memory reporting. VFS-backed filesystems use a common VFS adapter on top of their ESP-IDF backend.

Format timing measures the filesystem format call itself. The benchmark erases the partition before format for fairness, but that preformat erase is setup and is not included in filesystem format, mount, baseline, or churn totals.

Churn is deterministic. It creates, replaces, and deletes files until it has written about 8 MiB of logical payload using seed `0x4f465346`. The fixed live target is 2,308,848 B with 128 KiB fill/free slack. Current churn writes 8,391,942 B through 346 creates, 114 replaces, and 223 explicit deletes. Average live file count is sampled at every mutation event and is 114 files over 683 samples. The final live set is 123 files and 2,434,361 B: 116 small 10-20 KiB files totaling 1,813,188 B, 6 medium 20-60 KiB files totaling 262,773 B, and 1 large 350 KiB file.

Churn total wall time comes from an explicit wall-clock timer around the churn core. It includes foreground writes, deletes, any adapter-provided GC steps, benchmark model/stat work, and log printing. The accounting rows split that wall time into measured write/delete/GC/benchmark-overhead buckets plus a residual `Churn unaccounted time`. `Churn benchmark overhead` is mostly verbose log printing, not filesystem work.

FASTFFS scheduled/debt-GC variants run GC between foreground operations, but that GC time is still inside churn wall time. FASTFFS no-GC/inline rows do not run benchmark-scheduled GC between operations; they rely on whatever cleanup the filesystem performs as part of foreground writes.

Memory rows are measured by `benchfs`. `FS base memory` is the mounted-filesystem resident heap/static-equivalent footprint reported by the adapter or measured as mount heap delta for VFS backends. `FS open file memory` is one open file slot/handle. `FS stack memory` is stack used over a no-op benchmark baseline and includes the downstream ESP partition/SPI flash call path. A raw partition side probe saw up to 668 B over baseline, so engineering stack budgets include that as part of the filesystem path worst case.

Filesystem implementation memory/buffer comparison:

| Filesystem/config | Base estimate | Base measured | Per open file estimate | Per open file measured | Stack measured |
|---|---:|---:|---:|---:|---:|
| FASTFFS default/hash-cache | 4,872 B:<br>136 B `struct fffs`<br>4,096 B hash-head index cache (1,024 entries x 4 B; configurable, optional)<br>512 B scratch (configurable)<br>128 B allocation bitmap (32 x 32-bit words; configurable, optional) | 4,872 B | 376 B `struct fffs_file` | 376 B | 1,116-1,164 B across current variants |
| FASTFFS minimal | 256 B:<br>128 B `struct fffs`<br>128 B scratch<br>No hash-head index cache<br>No allocation bitmap | 256 B | 184 B `struct fffs_file` with 64 B file cache | 184 B | 1,156 B |
| JesFS | 164 B global `SFLASH_INFO`<br>Includes 128 B internal flash working buffer | 164 B | 28 B `FS_DESC` | 28 B | 1,164 B |
| LittleFS | About 1,424 B:<br>1,152 B read/program/lookahead buffers<br>about 128 B `lfs_t`<br>about 128 B wrapper<br>16 B initial FD pointer cache | 1,672 B | About 636 B VFS file wrapper/cache | 756 B | 1,452 B |
| SPIFFS | 3,124 B reserved with `.max_files = 8`:<br>512 B work buffer<br>20 B cache header<br>8 x 48 B file descriptors<br>8 x 276 B cache pages | 3,540 B | 324 B per configured/open slot:<br>48 B descriptor<br>276 B cache page | 344 B | 1,248 B |

For reference, raw flash partition timing for the ESP partition:

| Stat | Value |
|---|---:|
| Erase 4 KiB sector | 25.2 ms |
| Program 256-byte page, avg | 1.03 ms |
| Reprogram 256-byte page, avg | 1.40 ms |
| Read 4 B | 66 us |
| Read 256 B | 88 us |
| Read 4 KiB | 556 us |
| Erase 64 KiB range | 31.3 ms |
| Erase + program 4 KiB, 256 B pages | 53.5 ms |
| Erase + program 4 KiB, 1 KiB chunks | 48.3 ms |

## Main Filesystem Comparison

Cell colors use per-row log-scale thresholds for directly comparable performance rows: green is near the best value, yellow is the middle of the row spread, and red is near the worst value. Throughput rows are higher-is-better; time, latency, and storage-overhead rows are lower-is-better. Configuration, workload, accounting, and memory rows are left uncolored.

The main comparison uses FASTFFS default debt-GC as the recommended/default operating point and FASTFFS minimal debt-GC to show the lower-memory tradeoff. Additional variants are in the table below.

<table>
  <thead>
    <tr>
      <th align="left">Stat</th>
      <th align="right">FASTFFS-default-debt-GC</th>
      <th align="right">FASTFFS-minimal-debt-GC</th>
      <th align="right">LittleFS</th>
      <th align="right">JesFS</th>
      <th align="right">SPIFFS</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td align="left">Run timestamp</td>
      <td align="right">2026-05-18 18:03:27</td>
      <td align="right">2026-05-18 19:56:33</td>
      <td align="right">2026-05-18 17:49:20</td>
      <td align="right">2026-05-18 16:37:44</td>
      <td align="right">2026-05-18 16:41:19</td>
    </tr>
    <tr>
      <td align="left">Index cache mode</td>
      <td align="right">1</td>
      <td align="right">0</td>
      <td align="right"></td>
      <td align="right"></td>
      <td align="right"></td>
    </tr>
    <tr>
      <td align="left">Index entries</td>
      <td align="right">1024</td>
      <td align="right">0</td>
      <td align="right"></td>
      <td align="right"></td>
      <td align="right"></td>
    </tr>
    <tr>
      <td align="left">Allocation map mode</td>
      <td align="right">1</td>
      <td align="right">0</td>
      <td align="right"></td>
      <td align="right"></td>
      <td align="right"></td>
    </tr>
    <tr>
      <td align="left">Allocation map words</td>
      <td align="right">32</td>
      <td align="right">0</td>
      <td align="right"></td>
      <td align="right"></td>
      <td align="right"></td>
    </tr>
    <tr>
      <td align="left">Scratch bytes</td>
      <td align="right">512</td>
      <td align="right">128</td>
      <td align="right"></td>
      <td align="right"></td>
      <td align="right"></td>
    </tr>
    <tr>
      <td align="left">File write buffer</td>
      <td align="right">256</td>
      <td align="right">64</td>
      <td align="right"></td>
      <td align="right"></td>
      <td align="right"></td>
    </tr>
    <tr>
      <td align="left">Churn GC policy</td>
      <td align="right">debt</td>
      <td align="right">debt</td>
      <td align="right"></td>
      <td align="right"></td>
      <td align="right"></td>
    </tr>
    <tr>
      <td align="left">Baseline format</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">65.3 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">48.5 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">42.8 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.892 s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">41.375 s</td>
    </tr>
    <tr>
      <td align="left">Baseline mount after format</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">348 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">278 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">529 us</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">63.9 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">230 ms</td>
    </tr>
    <tr>
      <td align="left">Reported usable capacity</td>
      <td align="right">4,173,848</td>
      <td align="right">4,173,848</td>
      <td align="right">4,194,304</td>
      <td align="right">4,194,304</td>
      <td align="right">3,848,081</td>
    </tr>
    <tr>
      <td align="left">Storage 192 x 64 B overhead/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">16</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">16</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">448</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">4,032</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">438</td>
    </tr>
    <tr>
      <td align="left">Write 192 x 64 B</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">20.8 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">10.1 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">0.59 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.93 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">0.27 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 192 x 64 B total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">179.2 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">35.6 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1.95 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">5.21 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">2.71 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 192 x 64 B open/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">178 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.52 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">11.1 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">11.9 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">22.9 ms</td>
    </tr>
    <tr>
      <td align="left">Read 192 x 64 B after open</td>
      <td align="right">23,810 KiB/s</td>
      <td align="right">963.9 KiB/s</td>
      <td align="right">5.99 KiB/s</td>
      <td align="right">476.3 KiB/s</td>
      <td align="right">416.9 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 32 early-index x 64 B total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">177.4 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">24.0 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">4.93 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">27.9 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">20.8 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 32 middle-index x 64 B total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">175.4 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">36.1 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">2.00 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">5.05 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">99.2 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 32 late-index x 64 B total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">176.4 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">75.0 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1.07 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">2.77 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">99.0 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Storage 208 mixed files overhead/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">16</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">16</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">571</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">3,879</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">443</td>
    </tr>
    <tr>
      <td align="left">Write 16 x 50 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">174.6 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">122.2 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">91.8 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">72.9 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">65.8 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 16 x 50 KiB total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,404 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,357 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,116 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1,278 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">322.6 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 16 x 50 KiB open/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">201 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">321 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.90 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">25.2 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">36.2 ms</td>
    </tr>
    <tr>
      <td align="left">Read 16 x 50 KiB after open</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,575 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,575 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,832 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,598 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">421.6 KiB/s</td>
    </tr>
    <tr>
      <td align="left">List 208 files</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">33.3 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">111 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">306 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">26.4 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">123 ms</td>
    </tr>
    <tr>
      <td align="left">Baseline exists tiny existing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">164 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.50 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">20.0 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">12.0 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">24.5 ms</td>
    </tr>
    <tr>
      <td align="left">Baseline exists tiny missing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">78 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.36 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">16.6 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">26.4 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">121 ms</td>
    </tr>
    <tr>
      <td align="left">Baseline exists medium existing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">155 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">296 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">4.50 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">25.4 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">32.9 ms</td>
    </tr>
    <tr>
      <td align="left">Baseline exists medium missing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">61 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.35 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">16.6 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">26.4 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">121 ms</td>
    </tr>
    <tr>
      <td align="left">Churn ops</td>
      <td align="right">460</td>
      <td align="right">460</td>
      <td align="right">460</td>
      <td align="right">460</td>
      <td align="right">460</td>
    </tr>
    <tr>
      <td align="left">Churn bytes written</td>
      <td align="right">8,391,942</td>
      <td align="right">8,391,942</td>
      <td align="right">8,391,942</td>
      <td align="right">8,391,942</td>
      <td align="right">8,391,942</td>
    </tr>
    <tr>
      <td align="left">Churn final live bytes</td>
      <td align="right">2,434,361</td>
      <td align="right">2,434,361</td>
      <td align="right">2,434,361</td>
      <td align="right">2,434,361</td>
      <td align="right">2,434,361</td>
    </tr>
    <tr>
      <td align="left">Churn creates</td>
      <td align="right">346</td>
      <td align="right">346</td>
      <td align="right">346</td>
      <td align="right">346</td>
      <td align="right">346</td>
    </tr>
    <tr>
      <td align="left">Churn replaces</td>
      <td align="right">114</td>
      <td align="right">114</td>
      <td align="right">114</td>
      <td align="right">114</td>
      <td align="right">114</td>
    </tr>
    <tr>
      <td align="left">Churn deletes</td>
      <td align="right">223</td>
      <td align="right">223</td>
      <td align="right">223</td>
      <td align="right">223</td>
      <td align="right">223</td>
    </tr>
    <tr>
      <td align="left">Churn average live files</td>
      <td align="right">114</td>
      <td align="right">114</td>
      <td align="right">114</td>
      <td align="right">114</td>
      <td align="right">114</td>
    </tr>
    <tr>
      <td align="left">Churn total wall time</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">107.281 s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">152.385 s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">158.943 s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">161.230 s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">755.656 s</td>
    </tr>
    <tr>
      <td align="left">Churn accounted time</td>
      <td align="right">106.723 s</td>
      <td align="right">152.116 s</td>
      <td align="right">158.933 s</td>
      <td align="right">161.225 s</td>
      <td align="right">755.644 s</td>
    </tr>
    <tr>
      <td align="left">Churn write time</td>
      <td align="right">57.225 s</td>
      <td align="right">100.625 s</td>
      <td align="right">149.279 s</td>
      <td align="right">155.486 s</td>
      <td align="right">729.449 s</td>
    </tr>
    <tr>
      <td align="left">Churn delete time</td>
      <td align="right">1.259 s</td>
      <td align="right">1.821 s</td>
      <td align="right">8.392 s</td>
      <td align="right">4.448 s</td>
      <td align="right">24.933 s</td>
    </tr>
    <tr>
      <td align="left">Churn GC step time</td>
      <td align="right">43.797 s</td>
      <td align="right">45.626 s</td>
      <td align="right">0.000 s</td>
      <td align="right">0.000 s</td>
      <td align="right">0.000 s</td>
    </tr>
    <tr>
      <td align="left">Churn benchmark overhead</td>
      <td align="right">4.442 s</td>
      <td align="right">4.043 s</td>
      <td align="right">1.262 s</td>
      <td align="right">1.291 s</td>
      <td align="right">1.262 s</td>
    </tr>
    <tr>
      <td align="left">Churn unaccounted time</td>
      <td align="right">0.558 s</td>
      <td align="right">0.269 s</td>
      <td align="right">0.010 s</td>
      <td align="right">0.005 s</td>
      <td align="right">0.012 s</td>
    </tr>
    <tr>
      <td align="left">Churn write 10-20 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">139.8 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">77.3 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">51.2 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">52.1 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">11.5 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write new 10-20 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">140.4 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">77.1 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">50.3 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">52.4 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">10.9 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write replace 10-20 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">138.0 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">78.0 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">54.0 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">51.1 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">13.8 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write 20-60 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">155.7 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">97.0 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">71.8 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">56.9 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">11.6 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write new 20-60 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">155.8 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">95.5 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">69.7 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">57.6 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">10.4 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write replace 20-60 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">155.6 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">101.5 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">78.1 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">55.0 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">16.7 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write 350 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">159.3 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">113.3 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">78.7 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">48.4 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">7.32 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn delete avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">5.64 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">8.17 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">37.6 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">19.9 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">112 ms</td>
    </tr>
    <tr>
      <td align="left">Churn delete p50</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">5.88 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">7.52 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">16.1 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">20.7 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">103 ms</td>
    </tr>
    <tr>
      <td align="left">Churn delete p95</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">11.3 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">15.2 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">26.4 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">31.7 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">235 ms</td>
    </tr>
    <tr>
      <td align="left">Churn delete max</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">15.1 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">18.4 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">742 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">38.4 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">427 ms</td>
    </tr>
    <tr>
      <td align="left">Churn GC steps</td>
      <td align="right">4,816</td>
      <td align="right">4,987</td>
      <td align="right">0</td>
      <td align="right">0</td>
      <td align="right">0</td>
    </tr>
    <tr>
      <td align="left">Churn GC erased sectors</td>
      <td align="right">1,161</td>
      <td align="right">1,154</td>
      <td align="right">0</td>
      <td align="right">0</td>
      <td align="right">0</td>
    </tr>
    <tr>
      <td align="left">Churn final list, 123 files</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">18.7 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">281 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">302 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">17.8 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">116 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold mount</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">2.04 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">2.37 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">14.9 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">81.1 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">230 ms</td>
    </tr>
    <tr>
      <td align="left">Churn final cold list, 123 files</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">18.7 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">281 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">302 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">17.8 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">116 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 10-20 KiB total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,055 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,280 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">577.8 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1,047 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">240.3 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 10-20 KiB open/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">202 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.98 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">6.99 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">9.79 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">54.0 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 10-20 KiB after open</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,536 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,530 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1,338 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,610 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,357 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 20-60 KiB total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,354 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,050 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1,562 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1,819 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">323.6 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 20-60 KiB open/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">202 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">886 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">4.68 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">11.5 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">45.8 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 20-60 KiB after open</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,547 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,522 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,400 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,579 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">496.3 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 350 KiB total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,532 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,456 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,380 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,037 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">509.6 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 350 KiB open/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">201 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">933 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">4.77 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">17.1 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">79.4 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 350 KiB after open</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,582 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,546 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,555 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,584 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">576.8 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold exists existing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">155 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.11 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">8.66 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">9.91 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">49.7 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold exists missing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">83 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">7.61 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.88 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">17.8 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">115 ms</td>
    </tr>
    <tr>
      <td align="left">FS base memory</td>
      <td align="right">4,872</td>
      <td align="right">256</td>
      <td align="right">1,672</td>
      <td align="right">164</td>
      <td align="right">3,540</td>
    </tr>
    <tr>
      <td align="left">FS open file memory</td>
      <td align="right">376</td>
      <td align="right">184</td>
      <td align="right">756</td>
      <td align="right">28</td>
      <td align="right">344</td>
    </tr>
    <tr>
      <td align="left">FS stack memory</td>
      <td align="right">1,156</td>
      <td align="right">1,156</td>
      <td align="right">1,452</td>
      <td align="right">1,164</td>
      <td align="right">1,248</td>
    </tr>
  </tbody>
</table>

FASTFFS preloads a small amount of file data during open, so the tiny-file after-open read row is effectively served from RAM for FASTFFS. The total read row and open/file row are the fair tiny-file comparison points.

## FASTFFS Variant Comparison

| Stat | default-inline-GC | default-debt-GC | inline-GC | noalloc-inline-GC | noalloc-debt-GC | minimal-inline-GC | minimal-debt-GC |
|---|---:|---:|---:|---:|---:|---:|---:|
| Run timestamp | 2026-05-18 17:59:17 | 2026-05-18 18:03:27 | 2026-05-18 18:05:51 | 2026-05-18 18:10:01 | 2026-05-18 18:15:11 | 2026-05-18 19:50:55 | 2026-05-18 19:56:33 |
| Index cache mode | 1 | 1 | 1 | 1 | 1 | 0 | 0 |
| Index entries | 1024 | 1024 | 1024 | 1024 | 1024 | 0 | 0 |
| Allocation map mode | 1 | 1 | 1 | 0 | 0 | 0 | 0 |
| Allocation map words | 32 | 32 | 32 | 0 | 0 | 0 | 0 |
| Scratch bytes | 512 | 512 | 512 | 512 | 512 | 128 | 128 |
| File write buffer | 256 | 256 | 256 | 256 | 256 | 64 | 64 |
| Churn GC policy | none | debt | none | none | debt | none | debt |
| Baseline format | 66.5 ms | 65.3 ms | 60.3 ms | 46.5 ms | 47.0 ms | 69.3 ms | 48.5 ms |
| Baseline mount after format | 353 us | 348 us | 353 us | 337 us | 338 us | 273 us | 278 us |
| Reported usable capacity | 4,173,848 | 4,173,848 | 4,173,848 | 4,173,848 | 4,173,848 | 4,173,848 | 4,173,848 |
| Storage 192 x 64 B overhead/file | 16 | 16 | 16 | 16 | 16 | 16 | 16 |
| Write 192 x 64 B | 20.8 KiB/s | 20.8 KiB/s | 20.8 KiB/s | 20.8 KiB/s | 20.8 KiB/s | 10.1 KiB/s | 10.1 KiB/s |
| Read 192 x 64 B total | 179.4 KiB/s | 179.2 KiB/s | 179.4 KiB/s | 179.1 KiB/s | 179.4 KiB/s | 35.7 KiB/s | 35.6 KiB/s |
| Read 192 x 64 B open/file | 178 us | 178 us | 178 us | 179 us | 178 us | 1.52 ms | 1.52 ms |
| Read 192 x 64 B after open | 22,222 KiB/s | 23,810 KiB/s | 22,727 KiB/s | 23,810 KiB/s | 23,810 KiB/s | 965.7 KiB/s | 963.9 KiB/s |
| Read 32 early-index x 64 B total | 177.4 KiB/s | 177.4 KiB/s | 177.5 KiB/s | 177.1 KiB/s | 177.6 KiB/s | 24.0 KiB/s | 24.0 KiB/s |
| Read 32 middle-index x 64 B total | 175.5 KiB/s | 175.4 KiB/s | 175.5 KiB/s | 175.4 KiB/s | 175.6 KiB/s | 36.1 KiB/s | 36.1 KiB/s |
| Read 32 late-index x 64 B total | 176.5 KiB/s | 176.4 KiB/s | 176.6 KiB/s | 176.3 KiB/s | 176.6 KiB/s | 75.1 KiB/s | 75.0 KiB/s |
| Storage 208 mixed files overhead/file | 16 | 16 | 16 | 16 | 16 | 16 | 16 |
| Write 16 x 50 KiB | 174.6 KiB/s | 174.6 KiB/s | 174.4 KiB/s | 164.8 KiB/s | 164.8 KiB/s | 122.6 KiB/s | 122.2 KiB/s |
| Read 16 x 50 KiB total | 4,406 KiB/s | 4,404 KiB/s | 4,404 KiB/s | 4,400 KiB/s | 4,405 KiB/s | 4,358 KiB/s | 4,357 KiB/s |
| Read 16 x 50 KiB open/file | 201 us | 201 us | 204 us | 201 us | 201 us | 319 us | 321 us |
| Read 16 x 50 KiB after open | 4,578 KiB/s | 4,575 KiB/s | 4,576 KiB/s | 4,572 KiB/s | 4,576 KiB/s | 4,575 KiB/s | 4,575 KiB/s |
| List 208 files | 33.2 ms | 33.3 ms | 33.2 ms | 33.3 ms | 33.2 ms | 111 ms | 111 ms |
| Baseline exists tiny existing avg | 164 us | 164 us | 164 us | 164 us | 163 us | 1.50 ms | 1.50 ms |
| Baseline exists tiny missing avg | 78 us | 78 us | 78 us | 78 us | 78 us | 2.36 ms | 2.36 ms |
| Baseline exists medium existing avg | 156 us | 155 us | 155 us | 155 us | 155 us | 296 us | 296 us |
| Baseline exists medium missing avg | 61 us | 61 us | 61 us | 61 us | 61 us | 2.34 ms | 2.35 ms |
| Churn ops | 460 | 460 | 460 | 460 | 460 | 460 | 460 |
| Churn bytes written | 8,391,942 | 8,391,942 | 8,391,942 | 8,391,942 | 8,391,942 | 8,391,942 | 8,391,942 |
| Churn final live bytes | 2,434,361 | 2,434,361 | 2,434,361 | 2,434,361 | 2,434,361 | 2,434,361 | 2,434,361 |
| Churn creates | 346 | 346 | 346 | 346 | 346 | 346 | 346 |
| Churn replaces | 114 | 114 | 114 | 114 | 114 | 114 | 114 |
| Churn deletes | 223 | 223 | 223 | 223 | 223 | 223 | 223 |
| Churn average live files | 114 | 114 | 114 | 114 | 114 | 114 | 114 |
| Churn total wall time | 213.562 s | 107.281 s | 213.692 s | 272.762 s | 132.995 s | 292.373 s | 152.385 s |
| Churn accounted time | 213.556 s | 106.723 s | 213.686 s | 272.756 s | 132.623 s | 292.367 s | 152.116 s |
| Churn write time | 210.394 s | 57.225 s | 210.525 s | 269.592 s | 82.882 s | 288.915 s | 100.625 s |
| Churn delete time | 0.686 s | 1.259 s | 0.686 s | 0.685 s | 1.271 s | 1.525 s | 1.821 s |
| Churn GC step time | 0.000 s | 43.797 s | 0.000 s | 0.000 s | 44.034 s | 0.000 s | 45.626 s |
| Churn benchmark overhead | 2.475 s | 4.442 s | 2.476 s | 2.479 s | 4.436 s | 1.927 s | 4.043 s |
| Churn unaccounted time | 0.006 s | 0.558 s | 0.006 s | 0.006 s | 0.372 s | 0.006 s | 0.269 s |
| Churn write 10-20 KiB | 39.7 KiB/s | 139.8 KiB/s | 39.7 KiB/s | 30.7 KiB/s | 93.3 KiB/s | 28.6 KiB/s | 77.3 KiB/s |
| Churn write new 10-20 KiB | 39.4 KiB/s | 140.4 KiB/s | 39.4 KiB/s | 30.5 KiB/s | 93.3 KiB/s | 28.4 KiB/s | 77.1 KiB/s |
| Churn write replace 10-20 KiB | 40.9 KiB/s | 138.0 KiB/s | 40.8 KiB/s | 31.4 KiB/s | 93.3 KiB/s | 29.3 KiB/s | 78.0 KiB/s |
| Churn write 20-60 KiB | 42.0 KiB/s | 155.7 KiB/s | 42.0 KiB/s | 33.8 KiB/s | 120.8 KiB/s | 31.4 KiB/s | 97.0 KiB/s |
| Churn write new 20-60 KiB | 43.5 KiB/s | 155.8 KiB/s | 43.5 KiB/s | 35.1 KiB/s | 118.9 KiB/s | 32.5 KiB/s | 95.5 KiB/s |
| Churn write replace 20-60 KiB | 38.3 KiB/s | 155.6 KiB/s | 38.3 KiB/s | 30.9 KiB/s | 126.2 KiB/s | 28.9 KiB/s | 101.5 KiB/s |
| Churn write 350 KiB | 23.3 KiB/s | 159.3 KiB/s | 23.3 KiB/s | 18.9 KiB/s | 144.3 KiB/s | 18.0 KiB/s | 113.3 KiB/s |
| Churn delete avg | 3.08 ms | 5.64 ms | 3.07 ms | 3.07 ms | 5.70 ms | 6.84 ms | 8.17 ms |
| Churn delete p50 | 1.87 ms | 5.88 ms | 1.87 ms | 1.87 ms | 5.20 ms | 4.69 ms | 7.52 ms |
| Churn delete p95 | 11.0 ms | 11.3 ms | 11.0 ms | 11.0 ms | 11.5 ms | 15.7 ms | 15.2 ms |
| Churn delete max | 14.6 ms | 15.1 ms | 14.5 ms | 14.5 ms | 14.8 ms | 21.2 ms | 18.4 ms |
| Churn GC steps | 0 | 4,816 | 0 | 0 | 4,987 | 0 | 4,987 |
| Churn GC erased sectors | 0 | 1,161 | 0 | 0 | 1,154 | 0 | 1,154 |
| Churn final list, 123 files | 18.8 ms | 18.7 ms | 18.8 ms | 18.8 ms | 18.7 ms | 281 ms | 281 ms |
| Churn cold mount | 2.03 ms | 2.04 ms | 2.03 ms | 2.02 ms | 2.01 ms | 2.35 ms | 2.37 ms |
| Churn final cold list, 123 files | 18.8 ms | 18.7 ms | 18.8 ms | 18.8 ms | 18.7 ms | 281 ms | 281 ms |
| Churn cold read 10-20 KiB total | 4,061 KiB/s | 4,055 KiB/s | 4,061 KiB/s | 4,057 KiB/s | 4,074 KiB/s | 2,273 KiB/s | 2,280 KiB/s |
| Churn cold read 10-20 KiB open/file | 200 us | 202 us | 200 us | 201 us | 201 us | 2.98 ms | 2.98 ms |
| Churn cold read 10-20 KiB after open | 4,541 KiB/s | 4,536 KiB/s | 4,542 KiB/s | 4,536 KiB/s | 4,556 KiB/s | 4,501 KiB/s | 4,530 KiB/s |
| Churn cold read 20-60 KiB total | 4,360 KiB/s | 4,354 KiB/s | 4,360 KiB/s | 4,354 KiB/s | 4,340 KiB/s | 4,072 KiB/s | 4,050 KiB/s |
| Churn cold read 20-60 KiB open/file | 204 us | 202 us | 204 us | 205 us | 203 us | 886 us | 886 us |
| Churn cold read 20-60 KiB after open | 4,555 KiB/s | 4,547 KiB/s | 4,555 KiB/s | 4,551 KiB/s | 4,534 KiB/s | 4,551 KiB/s | 4,522 KiB/s |
| Churn cold read 350 KiB total | 4,493 KiB/s | 4,532 KiB/s | 4,493 KiB/s | 4,489 KiB/s | 4,493 KiB/s | 4,458 KiB/s | 4,456 KiB/s |
| Churn cold read 350 KiB open/file | 201 us | 201 us | 201 us | 202 us | 201 us | 932 us | 933 us |
| Churn cold read 350 KiB after open | 4,541 KiB/s | 4,582 KiB/s | 4,542 KiB/s | 4,537 KiB/s | 4,542 KiB/s | 4,548 KiB/s | 4,546 KiB/s |
| Churn cold exists existing avg | 156 us | 155 us | 156 us | 156 us | 154 us | 2.11 ms | 2.11 ms |
| Churn cold exists missing avg | 83 us | 83 us | 83 us | 84 us | 84 us | 7.61 ms | 7.61 ms |
| FS base memory | 4,872 | 4,872 | 4,872 | 4,736 | 4,736 | 256 | 256 |
| FS open file memory | 376 | 376 | 376 | 376 | 376 | 184 | 184 |
| FS stack memory | 1,156 | 1,156 | 1,156 | 1,164 | 1,116 | 1,156 | 1,156 |

FASTFFS preloads a small amount of file data during open, so the tiny-file after-open read row is effectively served from RAM. Compare tiny reads primarily using total throughput and open/file time.

## Benchmark Takeaways


- Namespace representation matters more than the simple linear-vs-indexed distinction. A RAM index gives FASTFFS its best open/exists/list latency, but even the minimal configuration remains competitive because the on-flash namespace is a compact, sequential metadata log that is cheap to scan. Linear scans become painful when each candidate requires scattered metadata reads, sector-head walks, or larger filesystem metadata structures to find a file.
- Garbage collection is not something you want to occasionally stop the world for in the middle of a write. The GC tax has to be paid eventually; paying it while there are spare cycles is usually better than forcing a foreground write to do the work when may not be expecting it. The debt-GC simulation benchmarks show that dedicating some idle cycles improves overall efficiency, fewer cycles overall.
- Tiny-file packing is important for config/asset-heavy workloads. Sector-per-file layouts can be simple and RAM-efficient, but they waste a lot of flash when many files are tens of bytes to a few kilobytes.
- FASTFFS expands the set of configurable RAM accelerators available to the application. The index cache, allocation bitmap, scratch size, and file cache each target different costs, so a build can spend RAM on the bottlenecks that matter for its workload.
- Flash read bandwidth is rarely the limiting factor for these workloads after a file is open. Metadata topology, erase/reclaim behavior, and open/list path length dominate perceived performance.
- Filesystems that try to cover broad POSIX-style semantics and robust generalized usage patterns with one structure cannot optimize as directly for specific workloads.
