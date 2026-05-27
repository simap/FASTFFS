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

Churn is deterministic. It creates, replaces, and deletes files until it has written about 8.6 MiB of logical payload using seed `0x4f465346`. The fixed live target is 2,308,848 B with 128 KiB fill/free slack. Current churn writes 8,576,418 B through 169 creates, 59 replaces, and 64 explicit deletes. Average live file count is sampled at every mutation event and is 74 files over 292 samples. The final live set is 105 files and 2,414,932 B: 96 small 10-20 KiB files totaling 1,436,988 B, 7 medium 20-60 KiB files totaling 261,144 B, and 2 large 350 KiB files totaling 716,800 B.

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
      <td align="right">2026-05-26 22:00:02</td>
      <td align="right">2026-05-26 22:20:15</td>
      <td align="right">2026-05-26 22:23:31</td>
      <td align="right">2026-05-26 22:26:59</td>
      <td align="right">2026-05-26 22:30:39</td>
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
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">49.5 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">66.5 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">69.2 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.903 s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">41.788 s</td>
    </tr>
    <tr>
      <td align="left">Baseline mount after format</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">350 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">276 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">530 us</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">64.1 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">230 ms</td>
    </tr>
    <tr>
      <td align="left">Reported usable capacity</td>
      <td align="right">4,175,892</td>
      <td align="right">4,175,892</td>
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
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">18.7 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">9.66 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">0.59 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.93 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">0.27 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 192 x 64 B total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">179.5 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">35.7 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1.95 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">5.20 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">2.70 KiB/s</td>
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
      <td align="right">24,691 KiB/s</td>
      <td align="right">967.6 KiB/s</td>
      <td align="right">5.99 KiB/s</td>
      <td align="right">474.4 KiB/s</td>
      <td align="right">430.1 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 32 early-index x 64 B total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">177.7 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">24.0 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">4.92 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">27.8 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">20.9 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 32 middle-index x 64 B total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">175.7 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">36.1 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">2.00 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">5.03 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">101.9 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 32 late-index x 64 B total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">176.7 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">75.1 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1.07 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">2.77 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">101.8 KiB/s</td>
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
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">173.9 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">122.4 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">90.2 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">73.0 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">65.8 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 16 x 50 KiB total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,427 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,378 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,116 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1,274 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">322.3 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 16 x 50 KiB open/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">201 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">320 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.92 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">25.2 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">36.3 ms</td>
    </tr>
    <tr>
      <td align="left">Read 16 x 50 KiB after open</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,599 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,596 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,835 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,592 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">421.1 KiB/s</td>
    </tr>
    <tr>
      <td align="left">List 208 files</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">33.2 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">111 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">306 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">26.5 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">123 ms</td>
    </tr>
    <tr>
      <td align="left">Baseline exists tiny existing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">163 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.50 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">20.0 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">12.0 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">24.6 ms</td>
    </tr>
    <tr>
      <td align="left">Baseline exists tiny missing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">78 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.36 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">16.6 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">26.5 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">121 ms</td>
    </tr>
    <tr>
      <td align="left">Baseline exists medium existing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">155 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">295 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">4.50 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">25.5 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">32.9 ms</td>
    </tr>
    <tr>
      <td align="left">Baseline exists medium missing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">61 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.34 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">16.6 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">26.5 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">121 ms</td>
    </tr>
    <tr>
      <td align="left">Churn ops</td>
      <td align="right">228</td>
      <td align="right">228</td>
      <td align="right">228</td>
      <td align="right">228</td>
      <td align="right">228</td>
    </tr>
    <tr>
      <td align="left">Churn bytes written</td>
      <td align="right">8,576,418</td>
      <td align="right">8,576,418</td>
      <td align="right">8,576,418</td>
      <td align="right">8,576,418</td>
      <td align="right">8,576,418</td>
    </tr>
    <tr>
      <td align="left">Churn final live bytes</td>
      <td align="right">2,414,932</td>
      <td align="right">2,414,932</td>
      <td align="right">2,414,932</td>
      <td align="right">2,414,932</td>
      <td align="right">2,414,932</td>
    </tr>
    <tr>
      <td align="left">Churn creates</td>
      <td align="right">169</td>
      <td align="right">169</td>
      <td align="right">169</td>
      <td align="right">169</td>
      <td align="right">169</td>
    </tr>
    <tr>
      <td align="left">Churn replaces</td>
      <td align="right">59</td>
      <td align="right">59</td>
      <td align="right">59</td>
      <td align="right">59</td>
      <td align="right">59</td>
    </tr>
    <tr>
      <td align="left">Churn deletes</td>
      <td align="right">64</td>
      <td align="right">64</td>
      <td align="right">64</td>
      <td align="right">64</td>
      <td align="right">64</td>
    </tr>
    <tr>
      <td align="left">Churn average live files</td>
      <td align="right">74</td>
      <td align="right">74</td>
      <td align="right">74</td>
      <td align="right">74</td>
      <td align="right">74</td>
    </tr>
    <tr>
      <td align="left">Churn total wall time</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">106.457 s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">148.656 s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">125.214 s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">152.325 s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">756.491 s</td>
    </tr>
    <tr>
      <td align="left">Churn accounted time</td>
      <td align="right">106.300 s</td>
      <td align="right">148.630 s</td>
      <td align="right">125.210 s</td>
      <td align="right">152.322 s</td>
      <td align="right">756.488 s</td>
    </tr>
    <tr>
      <td align="left">Churn write time</td>
      <td align="right">67.304 s</td>
      <td align="right">109.634 s</td>
      <td align="right">122.133 s</td>
      <td align="right">148.934 s</td>
      <td align="right">729.674 s</td>
    </tr>
    <tr>
      <td align="left">Churn delete time</td>
      <td align="right">0.751 s</td>
      <td align="right">0.746 s</td>
      <td align="right">2.472 s</td>
      <td align="right">2.772 s</td>
      <td align="right">26.208 s</td>
    </tr>
    <tr>
      <td align="left">Churn GC step time</td>
      <td align="right">36.407 s</td>
      <td align="right">36.557 s</td>
      <td align="right">0.000 s</td>
      <td align="right">0.000 s</td>
      <td align="right">0.000 s</td>
    </tr>
    <tr>
      <td align="left">Churn benchmark overhead</td>
      <td align="right">1.839 s</td>
      <td align="right">1.692 s</td>
      <td align="right">0.604 s</td>
      <td align="right">0.617 s</td>
      <td align="right">0.606 s</td>
    </tr>
    <tr>
      <td align="left">Churn unaccounted time</td>
      <td align="right">0.156 s</td>
      <td align="right">0.026 s</td>
      <td align="right">0.004 s</td>
      <td align="right">0.002 s</td>
      <td align="right">0.004 s</td>
    </tr>
    <tr>
      <td align="left">Churn write 10-20 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">139.4 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">79.4 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">53.7 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">54.8 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">14.4 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write new 10-20 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">140.0 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">80.1 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">52.7 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">56.0 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">14.8 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write replace 10-20 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">137.8 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">77.5 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">57.0 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">51.6 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">13.2 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write 20-60 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">160.3 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">104.9 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">70.8 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">62.2 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">19.5 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write new 20-60 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">160.0 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">104.2 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">68.3 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">62.7 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">19.7 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write replace 20-60 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">161.7 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">108.3 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">85.7 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">60.0 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">18.7 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write 350 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">108.5 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">68.3 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">82.5 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">55.4 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">9.02 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn delete avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">11.7 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">11.7 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">38.6 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">43.3 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">409 ms</td>
    </tr>
    <tr>
      <td align="left">Churn delete p50</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">6.73 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">6.48 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">13.4 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">23.2 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">135 ms</td>
    </tr>
    <tr>
      <td align="left">Churn delete p95</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">42.8 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">41.2 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">24.7 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">170 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1.980 s</td>
    </tr>
    <tr>
      <td align="left">Churn delete max</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">48.8 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">46.5 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">699 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">175 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">2.018 s</td>
    </tr>
    <tr>
      <td align="left">Churn GC steps</td>
      <td align="right">3,312</td>
      <td align="right">3,312</td>
      <td align="right">0</td>
      <td align="right">0</td>
      <td align="right">0</td>
    </tr>
    <tr>
      <td align="left">Churn GC erased sectors</td>
      <td align="right">995</td>
      <td align="right">952</td>
      <td align="right">0</td>
      <td align="right">0</td>
      <td align="right">0</td>
    </tr>
    <tr>
      <td align="left">Churn final list</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">16.0 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">97.0 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">173 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">13.6 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">115 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold mount</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">6.76 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">7.82 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">7.74 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">77.3 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">230 ms</td>
    </tr>
    <tr>
      <td align="left">Churn final cold list</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">16.0 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">96.9 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">173 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">13.6 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">115 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 10-20 KiB total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,111 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,009 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">999.2 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1,630 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">239.5 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 10-20 KiB open/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">201 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.59 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">3.91 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">5.31 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">59.8 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 10-20 KiB after open</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,556 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,508 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1,992 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">3,579 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">2,361 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 20-60 KiB total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,335 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,692 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1,768 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,314 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">483.1 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 20-60 KiB open/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">200 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.68 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">3.35 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">5.70 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">23.2 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 20-60 KiB after open</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,548 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,530 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,606 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,579 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">689.1 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 350 KiB total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,508 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,501 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,560 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">3,193 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">828.2 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 350 KiB open/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">202 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">231 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.25 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">11.3 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">110 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 350 KiB after open</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,556 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,551 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,621 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,577 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1,120 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold exists existing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">155 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.90 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">3.77 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">5.49 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">53.9 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold exists missing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">74 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">3.31 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.04 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">13.6 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">115 ms</td>
    </tr>
    <tr>
      <td align="left">FS base memory</td>
      <td align="right">4,876</td>
      <td align="right">260</td>
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
      <td align="right">80</td>
    </tr>
    <tr>
      <td align="left">FS stack memory</td>
      <td align="right">1,116</td>
      <td align="right">1,156</td>
      <td align="right">1,404</td>
      <td align="right">1,164</td>
      <td align="right">1,240</td>
    </tr>
  </tbody>
</table>

FASTFFS preloads a small amount of file data during open, so the tiny-file after-open read row is effectively served from RAM for FASTFFS. The total read row and open/file row are the fair tiny-file comparison points.

## FASTFFS Variant Comparison

| Stat | default-inline-GC | default-debt-GC | inline-GC | noalloc-inline-GC | noalloc-debt-GC | minimal-inline-GC | minimal-debt-GC |
|---|---:|---:|---:|---:|---:|---:|---:|
| Run timestamp | 2026-05-26 21:55:56 | 2026-05-26 22:00:02 | 2026-05-26 22:02:19 | 2026-05-26 22:06:37 | 2026-05-26 22:11:48 | 2026-05-26 22:14:42 | 2026-05-26 22:20:15 |
| Index cache mode | 1 | 1 | 1 | 1 | 1 | 0 | 0 |
| Index entries | 1024 | 1024 | 1024 | 1024 | 1024 | 0 | 0 |
| Allocation map mode | 1 | 1 | 1 | 0 | 0 | 0 | 0 |
| Allocation map words | 32 | 32 | 32 | 0 | 0 | 0 | 0 |
| Scratch bytes | 512 | 512 | 512 | 512 | 512 | 128 | 128 |
| File write buffer | 256 | 256 | 256 | 256 | 256 | 64 | 64 |
| Churn GC policy | none | debt | none | none | debt | none | debt |
| Baseline format | 58.1 ms | 49.5 ms | 49.6 ms | 49.3 ms | 48.8 ms | 48.8 ms | 66.5 ms |
| Baseline mount after format | 364 us | 350 us | 357 us | 339 us | 337 us | 289 us | 276 us |
| Reported usable capacity | 4,175,892 | 4,175,892 | 4,175,892 | 4,175,892 | 4,175,892 | 4,175,892 | 4,175,892 |
| Storage 192 x 64 B overhead/file | 16 | 16 | 16 | 16 | 16 | 16 | 16 |
| Write 192 x 64 B | 18.7 KiB/s | 18.7 KiB/s | 18.7 KiB/s | 18.7 KiB/s | 18.7 KiB/s | 9.66 KiB/s | 9.66 KiB/s |
| Read 192 x 64 B total | 179.2 KiB/s | 179.5 KiB/s | 179.1 KiB/s | 179.7 KiB/s | 179.7 KiB/s | 35.7 KiB/s | 35.7 KiB/s |
| Read 192 x 64 B open/file | 178 us | 178 us | 178 us | 178 us | 178 us | 1.52 ms | 1.52 ms |
| Read 192 x 64 B after open | 23,529 KiB/s | 24,691 KiB/s | 24,096 KiB/s | 23,810 KiB/s | 22,989 KiB/s | 963.4 KiB/s | 967.6 KiB/s |
| Read 32 early-index x 64 B total | 177.4 KiB/s | 177.7 KiB/s | 177.4 KiB/s | 177.5 KiB/s | 177.9 KiB/s | 24.0 KiB/s | 24.0 KiB/s |
| Read 32 middle-index x 64 B total | 175.5 KiB/s | 175.7 KiB/s | 175.4 KiB/s | 175.6 KiB/s | 175.8 KiB/s | 36.1 KiB/s | 36.1 KiB/s |
| Read 32 late-index x 64 B total | 176.3 KiB/s | 176.7 KiB/s | 176.3 KiB/s | 176.8 KiB/s | 176.8 KiB/s | 75.0 KiB/s | 75.1 KiB/s |
| Storage 208 mixed files overhead/file | 16 | 16 | 16 | 16 | 16 | 16 | 16 |
| Write 16 x 50 KiB | 174.5 KiB/s | 173.9 KiB/s | 173.9 KiB/s | 164.5 KiB/s | 164.4 KiB/s | 122.4 KiB/s | 122.4 KiB/s |
| Read 16 x 50 KiB total | 4,422 KiB/s | 4,427 KiB/s | 4,422 KiB/s | 4,427 KiB/s | 4,428 KiB/s | 4,376 KiB/s | 4,378 KiB/s |
| Read 16 x 50 KiB open/file | 202 us | 201 us | 202 us | 201 us | 201 us | 320 us | 320 us |
| Read 16 x 50 KiB after open | 4,594 KiB/s | 4,599 KiB/s | 4,595 KiB/s | 4,600 KiB/s | 4,601 KiB/s | 4,595 KiB/s | 4,596 KiB/s |
| List 208 files | 33.3 ms | 33.2 ms | 33.3 ms | 33.2 ms | 33.2 ms | 111 ms | 111 ms |
| Baseline exists tiny existing avg | 164 us | 163 us | 164 us | 164 us | 164 us | 1.50 ms | 1.50 ms |
| Baseline exists tiny missing avg | 78 us | 78 us | 78 us | 78 us | 78 us | 2.36 ms | 2.36 ms |
| Baseline exists medium existing avg | 155 us | 155 us | 155 us | 155 us | 155 us | 295 us | 295 us |
| Baseline exists medium missing avg | 61 us | 61 us | 61 us | 61 us | 61 us | 2.34 ms | 2.34 ms |
| Churn ops | 228 | 228 | 228 | 228 | 228 | 228 | 228 |
| Churn bytes written | 8,576,418 | 8,576,418 | 8,576,418 | 8,576,418 | 8,576,418 | 8,576,418 | 8,576,418 |
| Churn final live bytes | 2,414,932 | 2,414,932 | 2,414,932 | 2,414,932 | 2,414,932 | 2,414,932 | 2,414,932 |
| Churn creates | 169 | 169 | 169 | 169 | 169 | 169 | 169 |
| Churn replaces | 59 | 59 | 59 | 59 | 59 | 59 | 59 |
| Churn deletes | 64 | 64 | 64 | 64 | 64 | 64 | 64 |
| Churn average live files | 74 | 74 | 74 | 74 | 74 | 74 | 74 |
| Churn total wall time | 214.753 s | 106.457 s | 214.832 s | 267.163 s | 130.367 s | 286.297 s | 148.656 s |
| Churn accounted time | 214.750 s | 106.300 s | 214.830 s | 267.160 s | 130.340 s | 286.295 s | 148.630 s |
| Churn write time | 213.200 s | 67.304 s | 213.287 s | 265.607 s | 92.095 s | 284.747 s | 109.634 s |
| Churn delete time | 0.675 s | 0.751 s | 0.668 s | 0.677 s | 0.700 s | 0.741 s | 0.746 s |
| Churn GC step time | 0.000 s | 36.407 s | 0.000 s | 0.000 s | 35.752 s | 0.000 s | 36.557 s |
| Churn benchmark overhead | 0.875 s | 1.839 s | 0.875 s | 0.876 s | 1.792 s | 0.807 s | 1.692 s |
| Churn unaccounted time | 0.003 s | 0.156 s | 0.003 s | 0.003 s | 0.027 s | 0.003 s | 0.026 s |
| Churn write 10-20 KiB | 43.2 KiB/s | 139.4 KiB/s | 43.2 KiB/s | 33.2 KiB/s | 95.8 KiB/s | 30.9 KiB/s | 79.4 KiB/s |
| Churn write new 10-20 KiB | 43.6 KiB/s | 140.0 KiB/s | 43.5 KiB/s | 33.6 KiB/s | 96.9 KiB/s | 31.2 KiB/s | 80.1 KiB/s |
| Churn write replace 10-20 KiB | 42.2 KiB/s | 137.8 KiB/s | 42.2 KiB/s | 32.3 KiB/s | 92.8 KiB/s | 30.0 KiB/s | 77.5 KiB/s |
| Churn write 20-60 KiB | 56.4 KiB/s | 160.3 KiB/s | 56.3 KiB/s | 45.8 KiB/s | 132.6 KiB/s | 41.7 KiB/s | 104.9 KiB/s |
| Churn write new 20-60 KiB | 57.1 KiB/s | 160.0 KiB/s | 57.0 KiB/s | 46.3 KiB/s | 131.6 KiB/s | 42.2 KiB/s | 104.2 KiB/s |
| Churn write replace 20-60 KiB | 53.3 KiB/s | 161.7 KiB/s | 53.2 KiB/s | 43.3 KiB/s | 137.6 KiB/s | 39.7 KiB/s | 108.3 KiB/s |
| Churn write 350 KiB | 33.8 KiB/s | 108.5 KiB/s | 33.8 KiB/s | 27.7 KiB/s | 79.7 KiB/s | 26.0 KiB/s | 68.3 KiB/s |
| Churn delete avg | 10.6 ms | 11.7 ms | 10.4 ms | 10.6 ms | 10.9 ms | 11.6 ms | 11.7 ms |
| Churn delete p50 | 4.28 ms | 6.73 ms | 4.29 ms | 4.24 ms | 5.10 ms | 5.93 ms | 6.48 ms |
| Churn delete p95 | 39.0 ms | 42.8 ms | 39.0 ms | 39.0 ms | 41.3 ms | 39.1 ms | 41.2 ms |
| Churn delete max | 46.5 ms | 48.8 ms | 46.8 ms | 48.9 ms | 48.8 ms | 42.8 ms | 46.5 ms |
| Churn GC steps | 0 | 3,312 | 0 | 0 | 3,312 | 0 | 3,312 |
| Churn GC erased sectors | 0 | 995 | 0 | 0 | 952 | 0 | 952 |
| Churn final list | 16.1 ms | 16.0 ms | 16.1 ms | 16.0 ms | 16.0 ms | 97.0 ms | 97.0 ms |
| Churn cold mount | 6.76 ms | 6.76 ms | 6.77 ms | 6.74 ms | 6.75 ms | 7.83 ms | 7.82 ms |
| Churn final cold list | 16.0 ms | 16.0 ms | 16.0 ms | 16.0 ms | 16.0 ms | 96.9 ms | 96.9 ms |
| Churn cold read 10-20 KiB total | 4,106 KiB/s | 4,111 KiB/s | 4,106 KiB/s | 4,110 KiB/s | 4,101 KiB/s | 3,019 KiB/s | 3,009 KiB/s |
| Churn cold read 10-20 KiB open/file | 202 us | 201 us | 202 us | 203 us | 202 us | 1.59 ms | 1.59 ms |
| Churn cold read 10-20 KiB after open | 4,551 KiB/s | 4,556 KiB/s | 4,551 KiB/s | 4,557 KiB/s | 4,545 KiB/s | 4,531 KiB/s | 4,508 KiB/s |
| Churn cold read 20-60 KiB total | 4,342 KiB/s | 4,335 KiB/s | 4,342 KiB/s | 4,348 KiB/s | 4,326 KiB/s | 3,702 KiB/s | 3,692 KiB/s |
| Churn cold read 20-60 KiB open/file | 199 us | 200 us | 199 us | 198 us | 198 us | 1.68 ms | 1.68 ms |
| Churn cold read 20-60 KiB after open | 4,557 KiB/s | 4,548 KiB/s | 4,557 KiB/s | 4,562 KiB/s | 4,538 KiB/s | 4,545 KiB/s | 4,530 KiB/s |
| Churn cold read 350 KiB total | 4,492 KiB/s | 4,508 KiB/s | 4,492 KiB/s | 4,497 KiB/s | 4,494 KiB/s | 4,500 KiB/s | 4,501 KiB/s |
| Churn cold read 350 KiB open/file | 202 us | 202 us | 201 us | 201 us | 196 us | 236 us | 231 us |
| Churn cold read 350 KiB after open | 4,540 KiB/s | 4,556 KiB/s | 4,540 KiB/s | 4,545 KiB/s | 4,542 KiB/s | 4,550 KiB/s | 4,551 KiB/s |
| Churn cold exists existing avg | 155 us | 155 us | 155 us | 155 us | 155 us | 1.90 ms | 1.90 ms |
| Churn cold exists missing avg | 74 us | 74 us | 74 us | 74 us | 74 us | 3.31 ms | 3.31 ms |
| FS base memory | 4,876 | 4,876 | 4,876 | 4,740 | 4,740 | 260 | 260 |
| FS open file memory | 376 | 376 | 376 | 376 | 376 | 184 | 184 |
| FS stack memory | 1,164 | 1,116 | 1,164 | 1,156 | 1,108 | 1,156 | 1,156 |

FASTFFS preloads a small amount of file data during open, so the tiny-file after-open read row is effectively served from RAM. Compare tiny reads primarily using total throughput and open/file time.

## Benchmark Takeaways


- Namespace representation matters more than the simple linear-vs-indexed distinction. A RAM index gives FASTFFS its best open/exists/list latency, but even the minimal configuration remains competitive because the on-flash namespace is a compact, sequential metadata log that is cheap to scan. Linear scans become painful when each candidate requires scattered metadata reads, sector-head walks, or larger filesystem metadata structures to find a file.
- Garbage collection is not something you want to occasionally stop the world for in the middle of a write. The GC tax has to be paid eventually; paying it while there are spare cycles is usually better than forcing a foreground write to do the work when may not be expecting it. The debt-GC simulation benchmarks show that dedicating some idle cycles improves overall efficiency, fewer cycles overall.
- Tiny-file packing is important for config/asset-heavy workloads. Sector-per-file layouts can be simple and RAM-efficient, but they waste a lot of flash when many files are tens of bytes to a few kilobytes.
- FASTFFS expands the set of configurable RAM accelerators available to the application. The index cache, allocation bitmap, scratch size, and file cache each target different costs, so a build can spend RAM on the bottlenecks that matter for its workload.
- Flash read bandwidth is rarely the limiting factor for these workloads after a file is open. Metadata topology, erase/reclaim behavior, and open/list path length dominate perceived performance.
- Filesystems that try to cover broad POSIX-style semantics and robust generalized usage patterns with one structure cannot optimize as directly for specific workloads.
