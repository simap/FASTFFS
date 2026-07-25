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
| FATFS + ESP-IDF WL | BSD-style FatFs license + Apache-2.0 ESP-IDF wear levelling | FAT filesystem over flash wear layer | Baseline | Drop-in ESP-IDF baseline; familiar filesystem, but 4 KiB allocation units are expensive for tiny files. |
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

Current likely-near qualifying candidate count: **one, JesFS**. FATFS is included as an ESP-IDF baseline, not as a close architectural match.

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

### FATFS + ESP-IDF Wear Levelling

The FATFS benchmark uses ESP-IDF's `esp_vfs_fat_spiflash_*_rw_wl` path, with FatFs mounted through the ESP-IDF wear-levelling layer. The benchmark adapter leaves wear-levelling mode at the ESP-IDF default and sets `.allocation_unit_size = CONFIG_WL_SECTOR_SIZE`, which is 4 KiB in the tested configuration. It uses two FAT tables (`use_one_fat = false`) and `.max_files = 8`.

This gives a useful "standard filesystem on flash" baseline. It also makes the tiny-file storage cost visible: each 64 B file consumes a 4 KiB cluster, and small file mutation has to update file data plus FAT/directory metadata through the wear-levelling layer.

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

Churn is deterministic. It creates, replaces, and deletes files until it has written about 8.2 MB of logical payload using seed `0x4f465346`. The fixed live target is 2,308,848 B with 128 KiB fill/free slack. Current churn writes 8,576,418 B through 169 creates, 59 replaces, and 64 explicit deletes. Average live file count is sampled at every mutation event and is 74 files over 292 samples. The final live set is 105 files and 2,414,932 B: 96 small 10-20 KiB files totaling 1,436,988 B, 7 medium 20-60 KiB files totaling 261,144 B, and 2 large 350 KiB files totaling 716,800 B.

Small-file churn is a separate deterministic phase. It uses only 1 B-5 KiB files, a 5,000-slot cap, 25% replace probability, seed `0x53464348`, an 8 MiB logical write target, the same 2,308,848 B live target as main churn, and 32 KiB slack. That is roughly a 60% live-payload target by model capacity, with slack allowing about 39% model free space before forced deletes. This phase exposes metadata pressure, tiny-file packing, and many-file open/list behavior. Filesystems that fail before the write target still report the partial progress point.

Churn total wall time comes from an explicit wall-clock timer around the churn core. It includes foreground writes, deletes, any adapter-provided GC steps, benchmark model/stat work, and log printing. The accounting rows split that wall time into measured write/delete/GC/benchmark-overhead buckets plus a residual `Churn unaccounted time`. `Churn benchmark overhead` is mostly verbose log printing, not filesystem work.

FASTFFS scheduled/debt-GC variants run GC between foreground operations, but that GC time is still inside churn wall time. FASTFFS no-GC/inline rows do not run benchmark-scheduled GC between operations; they rely on whatever cleanup the filesystem performs as part of foreground writes.

Memory rows are measured by `benchfs`. `FS base memory` is the mounted-filesystem resident heap/static-equivalent footprint reported by the adapter or measured as mount heap delta for VFS backends. `FS open file memory` is one open file slot/handle. `FS stack memory` is stack used over a no-op benchmark baseline and includes the downstream ESP partition/SPI flash call path. A raw partition side probe saw up to 668 B over baseline, so engineering stack budgets include that as part of the filesystem path worst case.

Filesystem implementation memory/buffer comparison:

| Filesystem/config | Base estimate | Base measured | Per open file estimate | Per open file measured | Stack measured |
|---|---:|---:|---:|---:|---:|
| FASTFFS default/hash-cache | About 4.9 KiB:<br>`struct fffs`<br>4,096 B hash-head index cache (1,024 entries x 4 B; configurable, optional)<br>512 B scratch (configurable)<br>128 B allocation bitmap (32 x 32-bit words; configurable, optional) | 4,924 B | `struct fffs_file` with 256 B file cache | 360 B | 1,284 B |
| FASTFFS minimal | About 308 B:<br>`struct fffs`<br>128 B scratch<br>No hash-head index cache<br>No allocation bitmap | 308 B | `struct fffs_file` with 64 B file cache | 168 B | 1,244 B |
| JesFS | 164 B global `SFLASH_INFO`<br>Includes 128 B internal flash working buffer | 164 B | 28 B `FS_DESC` | 28 B | 1,236 B |
| LittleFS | About 1,424 B:<br>1,152 B read/program/lookahead buffers<br>about 128 B `lfs_t`<br>about 128 B wrapper<br>16 B initial FD pointer cache | 1,672 B | About 636 B VFS file wrapper/cache | 756 B | 1,444 B |
| FATFS + ESP-IDF WL | ESP-IDF VFS/FatFs/WL state measured by mount heap delta | 9,996 B | Not separately reported by adapter |  | 1,344 B |
| SPIFFS | 3,124 B reserved with `.max_files = 8`:<br>512 B work buffer<br>20 B cache header<br>8 x 48 B file descriptors<br>8 x 276 B cache pages | 3,540 B | 324 B per configured/open slot:<br>48 B descriptor<br>276 B cache page | 80 B | 1,328 B |

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

The main comparison uses FASTFFS default debt-GC as the recommended/default operating point. This table is generated from June 2026 measurements for FASTFFS default debt-GC, FASTFFS minimal debt-GC, LittleFS, FATFS, JesFS, and SPIFFS with the current main churn and small-file churn workloads. A current FASTFFS variant comparison follows in the variant section.

<table>
  <thead>
    <tr>
      <th align="left">Stat</th>
      <th align="right">FASTFFS-default-debt-GC</th>
      <th align="right">FASTFFS-minimal-debt-GC</th>
      <th align="right">LittleFS</th>
      <th align="right">FATFS</th>
      <th align="right">JesFS</th>
      <th align="right">SPIFFS</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td align="left">Run timestamp</td>
      <td align="right">2026-06-06 10:20:53</td>
      <td align="right">2026-06-06 06:32:37</td>
      <td align="right">2026-06-05 07:16:51</td>
      <td align="right">2026-06-05 07:23:50</td>
      <td align="right">2026-06-05 08:28:59</td>
      <td align="right">2026-06-05 07:30:07</td>
    </tr>
    <tr>
      <td align="left">Index cache mode</td>
      <td align="right">1</td>
      <td align="right">0</td>
      <td align="right"></td>
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
      <td align="right"></td>
    </tr>
    <tr>
      <td align="left">Allocation map mode</td>
      <td align="right">1</td>
      <td align="right">0</td>
      <td align="right"></td>
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
      <td align="right"></td>
    </tr>
    <tr>
      <td align="left">Scratch bytes</td>
      <td align="right">512</td>
      <td align="right">128</td>
      <td align="right"></td>
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
      <td align="right"></td>
    </tr>
    <tr>
      <td align="left">Churn GC policy</td>
      <td align="right">debt</td>
      <td align="right">debt</td>
      <td align="right"></td>
      <td align="right"></td>
      <td align="right"></td>
      <td align="right"></td>
    </tr>
    <tr>
      <td align="left">Baseline format</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">42.0 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">42.4 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">42.3 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">559 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.895 s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">41.380 s</td>
    </tr>
    <tr>
      <td align="left">Baseline mount after format</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">359 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">271 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">526 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.07 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">63.9 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">230 ms</td>
    </tr>
    <tr>
      <td align="left">Reported usable capacity</td>
      <td align="right">4,175,892</td>
      <td align="right">4,175,892</td>
      <td align="right">4,194,304</td>
      <td align="right">4,116,480</td>
      <td align="right">4,194,304</td>
      <td align="right">3,848,081</td>
    </tr>
    <tr>
      <td align="left">Storage 192 x 64 B overhead/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">16</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">16</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">448</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">4,032</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">4,032</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">438</td>
    </tr>
    <tr>
      <td align="left">Write 192 x 64 B</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">15.0 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">8.64 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">0.59 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">0.23 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.93 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">0.27 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 192 x 64 B total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">148.1 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">33.8 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1.95 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">48.3 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">5.21 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">2.70 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 192 x 64 B open/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">259 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.62 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">11.1 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">580 us</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">11.9 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">22.9 ms</td>
    </tr>
    <tr>
      <td align="left">Read 192 x 64 B after open</td>
      <td align="right">23,529 KiB/s</td>
      <td align="right">963.4 KiB/s</td>
      <td align="right">5.99 KiB/s</td>
      <td align="right">101.9 KiB/s</td>
      <td align="right">475.5 KiB/s</td>
      <td align="right">428.8 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 32 early-index x 64 B total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">146.6 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">23.0 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">4.92 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">73.2 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">27.9 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">16.9 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 32 middle-index x 64 B total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">145.5 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">34.2 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">2.00 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">68.1 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">5.04 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">100.8 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 32 late-index x 64 B total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">146.0 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">68.5 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1.07 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">29.1 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">2.77 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">100.7 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Storage 208 mixed files overhead/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">16</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">16</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">571</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">3,879</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">3,879</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">443</td>
    </tr>
    <tr>
      <td align="left">Write 16 x 50 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">174.9 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">127.6 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">92.2 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">60.7 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">72.9 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">65.8 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 16 x 50 KiB total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,403 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,367 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,115 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,595 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1,276 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">322.5 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Read 16 x 50 KiB open/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">274 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">396 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.91 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.31 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">25.2 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">36.2 ms</td>
    </tr>
    <tr>
      <td align="left">Read 16 x 50 KiB after open</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,605 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,615 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,832 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">5,314 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,593 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">421.5 KiB/s</td>
    </tr>
    <tr>
      <td align="left">List 208 files</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">32.0 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">107 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">307 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">8.29 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">26.4 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">123 ms</td>
    </tr>
    <tr>
      <td align="left">Baseline exists tiny existing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">158 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.52 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">20.0 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">951 us</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">12.0 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">24.6 ms</td>
    </tr>
    <tr>
      <td align="left">Baseline exists tiny missing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">77 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.40 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">16.6 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.02 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">26.4 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">121 ms</td>
    </tr>
    <tr>
      <td align="left">Baseline exists medium existing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">150 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">291 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">4.50 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.44 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">25.5 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">32.9 ms</td>
    </tr>
    <tr>
      <td align="left">Baseline exists medium missing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">61 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.39 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">16.6 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.42 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">26.4 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">121 ms</td>
    </tr>
    <tr>
      <td align="left">Churn ops</td>
      <td align="right">228</td>
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
      <td align="right">8,576,418</td>
    </tr>
    <tr>
      <td align="left">Churn final live bytes</td>
      <td align="right">2,414,932</td>
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
      <td align="right">169</td>
    </tr>
    <tr>
      <td align="left">Churn replaces</td>
      <td align="right">59</td>
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
      <td align="right">64</td>
    </tr>
    <tr>
      <td align="left">Churn average live files</td>
      <td align="right">74</td>
      <td align="right">74</td>
      <td align="right">74</td>
      <td align="right">74</td>
      <td align="right">74</td>
      <td align="right">74</td>
    </tr>
    <tr>
      <td align="left">Churn total wall time</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">100.792 s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">133.416 s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">121.812 s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">183.485 s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">153.217 s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">757.174 s</td>
    </tr>
    <tr>
      <td align="left">Churn accounted time</td>
      <td align="right">100.789 s</td>
      <td align="right">133.413 s</td>
      <td align="right">121.806 s</td>
      <td align="right">183.479 s</td>
      <td align="right">153.215 s</td>
      <td align="right">757.167 s</td>
    </tr>
    <tr>
      <td align="left">Churn write time</td>
      <td align="right">60.907 s</td>
      <td align="right">93.908 s</td>
      <td align="right">117.847 s</td>
      <td align="right">170.920 s</td>
      <td align="right">149.401 s</td>
      <td align="right">729.957 s</td>
    </tr>
    <tr>
      <td align="left">Churn delete time</td>
      <td align="right">0.633 s</td>
      <td align="right">0.677 s</td>
      <td align="right">2.881 s</td>
      <td align="right">11.491 s</td>
      <td align="right">2.735 s</td>
      <td align="right">26.129 s</td>
    </tr>
    <tr>
      <td align="left">Churn GC step time</td>
      <td align="right">38.080 s</td>
      <td align="right">37.662 s</td>
      <td align="right">0.000 s</td>
      <td align="right">0.000 s</td>
      <td align="right">0.000 s</td>
      <td align="right">0.000 s</td>
    </tr>
    <tr>
      <td align="left">Churn benchmark overhead</td>
      <td align="right">1.170 s</td>
      <td align="right">1.166 s</td>
      <td align="right">1.078 s</td>
      <td align="right">1.068 s</td>
      <td align="right">1.079 s</td>
      <td align="right">1.082 s</td>
    </tr>
    <tr>
      <td align="left">Churn unaccounted time</td>
      <td align="right">0.003 s</td>
      <td align="right">0.004 s</td>
      <td align="right">0.006 s</td>
      <td align="right">0.006 s</td>
      <td align="right">0.002 s</td>
      <td align="right">0.006 s</td>
    </tr>
    <tr>
      <td align="left">Churn write 10-20 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">151.6 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">93.8 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">57.7 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">31.2 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">54.6 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">14.4 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write new 10-20 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">153.2 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">93.8 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">54.7 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">33.8 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">55.7 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">14.8 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write replace 10-20 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">147.2 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">93.7 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">68.1 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">25.7 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">51.5 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">13.2 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write 20-60 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">165.4 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">104.5 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">73.4 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">53.7 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">62.1 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">19.5 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write new 20-60 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">164.7 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">102.4 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">75.5 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">56.0 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">62.7 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">19.7 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write replace 20-60 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">168.8 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">115.4 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">64.9 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">45.0 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">59.4 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">18.7 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn write 350 KiB</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">123.0 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">82.4 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">82.6 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">74.2 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">55.2 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">9.02 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn delete avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">9.88 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">10.6 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">45.0 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">180 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">42.7 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">408 ms</td>
    </tr>
    <tr>
      <td align="left">Churn delete p50</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4.24 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">5.34 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">15.1 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">160 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">22.8 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">135 ms</td>
    </tr>
    <tr>
      <td align="left">Churn delete p95</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">40.3 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">37.9 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">382 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">257 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">168 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1.985 s</td>
    </tr>
    <tr>
      <td align="left">Churn delete max</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">41.4 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">42.2 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">528 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">296 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">174 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">2.026 s</td>
    </tr>
    <tr>
      <td align="left">Churn GC steps</td>
      <td align="right">3,312</td>
      <td align="right">3,312</td>
      <td align="right">0</td>
      <td align="right">0</td>
      <td align="right">0</td>
      <td align="right">0</td>
    </tr>
    <tr>
      <td align="left">Churn GC erased sectors</td>
      <td align="right">1,164</td>
      <td align="right">1,110</td>
      <td align="right">0</td>
      <td align="right">0</td>
      <td align="right">0</td>
      <td align="right">0</td>
    </tr>
    <tr>
      <td align="left">Smallfiles result</td>
      <td align="right">completed</td>
      <td align="right">completed</td>
      <td align="right">failed: NO_SPACE</td>
      <td align="right">failed: Permission denied</td>
      <td align="right">failed: ERROR</td>
      <td align="right">completed</td>
    </tr>
    <tr>
      <td align="left">Smallfiles ops</td>
      <td align="right">3244</td>
      <td align="right">3244</td>
      <td align="right">1061</td>
      <td align="right">237</td>
      <td align="right">1092</td>
      <td align="right">3244</td>
    </tr>
    <tr>
      <td align="left">Smallfiles bytes written</td>
      <td align="right">8,389,152</td>
      <td align="right">8,389,152</td>
      <td align="right">2,786,760</td>
      <td align="right">626,411</td>
      <td align="right">2,844,960</td>
      <td align="right">8,389,152</td>
    </tr>
    <tr>
      <td align="left">Smallfiles final live bytes</td>
      <td align="right">2,335,027</td>
      <td align="right">2,335,027</td>
      <td align="right">2,135,845</td>
      <td align="right">468,560</td>
      <td align="right">2,173,343</td>
      <td align="right">2,335,027</td>
    </tr>
    <tr>
      <td align="left">Smallfiles final live files</td>
      <td align="right">922</td>
      <td align="right">922</td>
      <td align="right">808</td>
      <td align="right">170</td>
      <td align="right">829</td>
      <td align="right">922</td>
    </tr>
    <tr>
      <td align="left">Smallfiles creates</td>
      <td align="right">2446</td>
      <td align="right">2446</td>
      <td align="right">808</td>
      <td align="right">170</td>
      <td align="right">829</td>
      <td align="right">2446</td>
    </tr>
    <tr>
      <td align="left">Smallfiles replaces</td>
      <td align="right">798</td>
      <td align="right">798</td>
      <td align="right">253</td>
      <td align="right">67</td>
      <td align="right">263</td>
      <td align="right">798</td>
    </tr>
    <tr>
      <td align="left">Smallfiles deletes</td>
      <td align="right">1524</td>
      <td align="right">1524</td>
      <td align="right">0</td>
      <td align="right">0</td>
      <td align="right">0</td>
      <td align="right">1524</td>
    </tr>
    <tr>
      <td align="left">Smallfiles written target</td>
      <td align="right">8,388,608</td>
      <td align="right">8,388,608</td>
      <td align="right">8,388,608</td>
      <td align="right">8,388,608</td>
      <td align="right">8,388,608</td>
      <td align="right">8,388,608</td>
    </tr>
    <tr>
      <td align="left">Smallfiles live target</td>
      <td align="right">2,308,848</td>
      <td align="right">2,308,848</td>
      <td align="right">2,308,848</td>
      <td align="right">2,308,848</td>
      <td align="right">2,308,848</td>
      <td align="right">2,308,848</td>
    </tr>
    <tr>
      <td align="left">Smallfiles file slots</td>
      <td align="right">5,000</td>
      <td align="right">5,000</td>
      <td align="right">5,000</td>
      <td align="right">5,000</td>
      <td align="right">5,000</td>
      <td align="right">5,000</td>
    </tr>
    <tr>
      <td align="left">Smallfiles total wall time</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">202.509 s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1355.535 s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">201.470 s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">75.850 s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">102.159 s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">2485.149 s</td>
    </tr>
    <tr>
      <td align="left">Smallfiles accounted time</td>
      <td align="right">202.451 s</td>
      <td align="right">1355.448 s</td>
      <td align="right">198.739 s</td>
      <td align="right">75.829 s</td>
      <td align="right">101.969 s</td>
      <td align="right">2484.757 s</td>
    </tr>
    <tr>
      <td align="left">Smallfiles write time</td>
      <td align="right">164.663 s</td>
      <td align="right">1218.012 s</td>
      <td align="right">198.398 s</td>
      <td align="right">75.732 s</td>
      <td align="right">101.685 s</td>
      <td align="right">2336.757 s</td>
    </tr>
    <tr>
      <td align="left">Smallfiles delete time</td>
      <td align="right">11.167 s</td>
      <td align="right">63.000 s</td>
      <td align="right">0.000 s</td>
      <td align="right">0.000 s</td>
      <td align="right">0.000 s</td>
      <td align="right">146.047 s</td>
    </tr>
    <tr>
      <td align="left">Smallfiles GC step time</td>
      <td align="right">24.769 s</td>
      <td align="right">72.658 s</td>
      <td align="right">0.000 s</td>
      <td align="right">0.000 s</td>
      <td align="right">0.000 s</td>
      <td align="right">0.000 s</td>
    </tr>
    <tr>
      <td align="left">Smallfiles benchmark overhead</td>
      <td align="right">1.852 s</td>
      <td align="right">1.778 s</td>
      <td align="right">0.341 s</td>
      <td align="right">0.097 s</td>
      <td align="right">0.283 s</td>
      <td align="right">1.953 s</td>
    </tr>
    <tr>
      <td align="left">Smallfiles unaccounted time</td>
      <td align="right">0.058 s</td>
      <td align="right">0.087 s</td>
      <td align="right">2.731 s</td>
      <td align="right">0.020 s</td>
      <td align="right">0.190 s</td>
      <td align="right">0.392 s</td>
    </tr>
    <tr>
      <td align="left">Smallfiles write 1-5120 B</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">49.8 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">6.73 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">13.7 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">8.08 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">27.3 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">3.51 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Smallfiles write new 1-5120 B</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">49.8 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">6.70 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">12.0 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">9.09 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">27.6 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">3.19 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Smallfiles write replace 1-5120 B</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">49.7 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">6.82 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">27.3 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">6.27 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">26.3 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">5.17 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Smallfiles delete avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">7.33 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">41.3 ms</td>
      <td align="right">0 us</td>
      <td align="right">0 us</td>
      <td align="right">0 us</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">95.8 ms</td>
    </tr>
    <tr>
      <td align="left">Smallfiles delete p50</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">1.08 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">11.8 ms</td>
      <td align="right">0 us</td>
      <td align="right">0 us</td>
      <td align="right">0 us</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">94.2 ms</td>
    </tr>
    <tr>
      <td align="left">Smallfiles delete p95</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">10.5 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">19.2 ms</td>
      <td align="right">0 us</td>
      <td align="right">0 us</td>
      <td align="right">0 us</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">171 ms</td>
    </tr>
    <tr>
      <td align="left">Smallfiles delete max</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">534 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">4.217 s</td>
      <td align="right">0 us</td>
      <td align="right">0 us</td>
      <td align="right">0 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">186 ms</td>
    </tr>
    <tr>
      <td align="left">Smallfiles GC steps</td>
      <td align="right">53,024</td>
      <td align="right">54,938</td>
      <td align="right">0</td>
      <td align="right">0</td>
      <td align="right">0</td>
      <td align="right">0</td>
    </tr>
    <tr>
      <td align="left">Smallfiles GC erased sectors</td>
      <td align="right">819</td>
      <td align="right">603</td>
      <td align="right">0</td>
      <td align="right">0</td>
      <td align="right">0</td>
      <td align="right">0</td>
    </tr>
    <tr>
      <td align="left">Smallfiles final list</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">381 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">258 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.33 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">299 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">190 us</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">104 ms</td>
    </tr>
    <tr>
      <td align="left">Smallfiles cold mount</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">5.53 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">6.19 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">116 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">6.71 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">167 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">230 ms</td>
    </tr>
    <tr>
      <td align="left">Smallfiles cold list</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">138 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1.457 s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1.513 s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">11.2 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">106 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">183 ms</td>
    </tr>
    <tr>
      <td align="left">Smallfiles cold read 1-5120 B total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">2,406 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">320.7 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">24.9 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">831.4 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">50.1 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">21.3 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Smallfiles cold read 1-5120 B open/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">280 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">6.07 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">35.5 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.85 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">53.5 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">99.9 ms</td>
    </tr>
    <tr>
      <td align="left">Smallfiles cold read 1-5120 B after open</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,736 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,517 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">73.8 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">2,527 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,562 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">2,121 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Smallfiles cold exists existing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">157 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">5.82 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">5.92 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">196 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.10 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">69.5 ms</td>
    </tr>
    <tr>
      <td align="left">Smallfiles cold exists missing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">467 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">10.7 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">758 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.88 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">105 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">169 ms</td>
    </tr>
    <tr>
      <td align="left">Churn final list</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">15.5 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">94.0 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">275 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">6.82 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">13.7 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">115 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold mount</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">6.80 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">7.81 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">13.4 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">14.7 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">77.2 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">231 ms</td>
    </tr>
    <tr>
      <td align="left">Churn final cold list</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">15.5 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">93.9 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">274 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">7.29 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">13.7 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">115 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 10-20 KiB total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,102 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,010 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">615.8 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,051 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1,632 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">239.7 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 10-20 KiB open/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">277 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.69 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">7.20 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.54 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">5.30 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">59.8 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 10-20 KiB after open</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,640 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,632 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1,407 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,470 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">3,579 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,365 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 20-60 KiB total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,339 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,698 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1,460 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,363 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,315 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">483.4 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 20-60 KiB open/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">274 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.80 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">4.83 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.23 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">5.69 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">23.2 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 20-60 KiB after open</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,591 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,603 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,359 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">5,191 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">3,579 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">689.6 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 350 KiB total</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,517 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,528 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,554 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">5,662 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">3,194 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">828.8 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 350 KiB open/file</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">274 us</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">308 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.25 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">711 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">11.3 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">109 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold read 350 KiB after open</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,571 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">4,583 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2,617 KiB/s</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">5,781 KiB/s</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">3,578 KiB/s</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">1,121 KiB/s</td>
    </tr>
    <tr>
      <td align="left">Churn cold exists existing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">151 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.92 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">4.86 ms</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">201 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">5.48 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">53.9 ms</td>
    </tr>
    <tr>
      <td align="left">Churn cold exists missing avg</td>
      <td align="right" bgcolor="#d9ead3" style="background-color: #d9ead3;">50 us</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">3.35 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">1.19 ms</td>
      <td align="right" bgcolor="#fff2cc" style="background-color: #fff2cc;">2.11 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">13.6 ms</td>
      <td align="right" bgcolor="#f4cccc" style="background-color: #f4cccc;">115 ms</td>
    </tr>
    <tr>
      <td align="left">FS base memory</td>
      <td align="right">4,924</td>
      <td align="right">308</td>
      <td align="right">1,672</td>
      <td align="right">9,996</td>
      <td align="right">164</td>
      <td align="right">3,540</td>
    </tr>
    <tr>
      <td align="left">FS open file memory</td>
      <td align="right">360</td>
      <td align="right">168</td>
      <td align="right">756</td>
      <td align="right"></td>
      <td align="right">28</td>
      <td align="right">80</td>
    </tr>
    <tr>
      <td align="left">FS stack memory</td>
      <td align="right">1,284</td>
      <td align="right">1,244</td>
      <td align="right">1,444</td>
      <td align="right">1,344</td>
      <td align="right">1,236</td>
      <td align="right">1,328</td>
    </tr>
  </tbody>
</table>

FASTFFS preloads a small amount of file data during open, so the tiny-file after-open read row is effectively served from RAM for FASTFFS. The total read row and open/file row are the fair tiny-file comparison points.

## FASTFFS Variant Comparison

This variant table compares June 6, 2026 FASTFFS measurements across default, no-allocation-map, and minimal-RAM configurations with inline and debt GC policies.

| Stat | default-inline-GC | default-debt-GC | noalloc-inline-GC | noalloc-debt-GC | minimal-inline-GC | minimal-debt-GC |
|---|---:|---:|---:|---:|---:|---:|
| Run timestamp | 2026-06-06 10:26:48 | 2026-06-06 10:20:53 | 2026-06-06 10:33:13 | 2026-06-06 10:45:43 | 2026-06-06 10:57:00 | 2026-06-06 06:32:37 |
| Index cache mode | 1 | 1 | 1 | 1 | 0 | 0 |
| Index entries | 1024 | 1024 | 1024 | 1024 | 0 | 0 |
| Allocation map mode | 1 | 1 | 0 | 0 | 0 | 0 |
| Allocation map words | 32 | 32 | 0 | 0 | 0 | 0 |
| Scratch bytes | 512 | 512 | 512 | 512 | 128 | 128 |
| File write buffer | 256 | 256 | 256 | 256 | 64 | 64 |
| Churn GC policy | none | debt | none | debt | none | debt |
| Baseline format | 41.2 ms | 42.0 ms | 40.4 ms | 49.1 ms | 66.6 ms | 42.4 ms |
| Baseline mount after format | 355 us | 359 us | 330 us | 338 us | 273 us | 271 us |
| Reported usable capacity | 4,175,892 | 4,175,892 | 4,175,892 | 4,175,892 | 4,175,892 | 4,175,892 |
| Storage 192 x 64 B overhead/file | 16 | 16 | 16 | 16 | 16 | 16 |
| Write 192 x 64 B | 14.9 KiB/s | 15.0 KiB/s | 14.9 KiB/s | 14.9 KiB/s | 8.54 KiB/s | 8.64 KiB/s |
| Read 192 x 64 B total | 148.3 KiB/s | 148.1 KiB/s | 148.8 KiB/s | 148.6 KiB/s | 33.8 KiB/s | 33.8 KiB/s |
| Read 192 x 64 B open/file | 258 us | 259 us | 257 us | 257 us | 1.62 ms | 1.62 ms |
| Read 192 x 64 B after open | 23,810 KiB/s | 23,529 KiB/s | 22,222 KiB/s | 22,472 KiB/s | 964.3 KiB/s | 963.4 KiB/s |
| Read 32 early-index x 64 B total | 146.7 KiB/s | 146.6 KiB/s | 147.2 KiB/s | 146.9 KiB/s | 23.0 KiB/s | 23.0 KiB/s |
| Read 32 middle-index x 64 B total | 145.6 KiB/s | 145.5 KiB/s | 146.0 KiB/s | 145.8 KiB/s | 34.2 KiB/s | 34.2 KiB/s |
| Read 32 late-index x 64 B total | 146.3 KiB/s | 146.0 KiB/s | 146.5 KiB/s | 146.3 KiB/s | 68.6 KiB/s | 68.5 KiB/s |
| Storage 208 mixed files overhead/file | 16 | 16 | 16 | 16 | 16 | 16 |
| Write 16 x 50 KiB | 174.8 KiB/s | 174.9 KiB/s | 168.1 KiB/s | 168.4 KiB/s | 127.8 KiB/s | 127.6 KiB/s |
| Read 16 x 50 KiB total | 4,407 KiB/s | 4,403 KiB/s | 4,411 KiB/s | 4,410 KiB/s | 4,369 KiB/s | 4,367 KiB/s |
| Read 16 x 50 KiB open/file | 276 us | 274 us | 274 us | 274 us | 393 us | 396 us |
| Read 16 x 50 KiB after open | 4,608 KiB/s | 4,605 KiB/s | 4,612 KiB/s | 4,609 KiB/s | 4,616 KiB/s | 4,615 KiB/s |
| List 208 files | 31.9 ms | 32.0 ms | 31.9 ms | 31.9 ms | 107 ms | 107 ms |
| Baseline exists tiny existing avg | 157 us | 158 us | 157 us | 157 us | 1.52 ms | 1.52 ms |
| Baseline exists tiny missing avg | 77 us | 77 us | 77 us | 77 us | 2.40 ms | 2.40 ms |
| Baseline exists medium existing avg | 150 us | 150 us | 150 us | 150 us | 291 us | 291 us |
| Baseline exists medium missing avg | 60 us | 61 us | 60 us | 60 us | 2.38 ms | 2.39 ms |
| Churn ops | 228 | 228 | 228 | 228 | 228 | 228 |
| Churn bytes written | 8,576,418 | 8,576,418 | 8,576,418 | 8,576,418 | 8,576,418 | 8,576,418 |
| Churn final live bytes | 2,414,932 | 2,414,932 | 2,414,932 | 2,414,932 | 2,414,932 | 2,414,932 |
| Churn creates | 169 | 169 | 169 | 169 | 169 | 169 |
| Churn replaces | 59 | 59 | 59 | 59 | 59 | 59 |
| Churn deletes | 64 | 64 | 64 | 64 | 64 | 64 |
| Churn average live files | 74 | 74 | 74 | 74 | 74 | 74 |
| Churn total wall time | 131.784 s | 100.792 s | 189.474 s | 116.888 s | 201.884 s | 133.416 s |
| Churn accounted time | 131.781 s | 100.789 s | 189.471 s | 116.885 s | 201.882 s | 133.413 s |
| Churn write time | 129.804 s | 60.907 s | 187.491 s | 77.691 s | 199.896 s | 93.908 s |
| Churn delete time | 0.601 s | 0.633 s | 0.598 s | 0.618 s | 0.681 s | 0.677 s |
| Churn GC step time | 0.000 s | 38.080 s | 0.000 s | 37.407 s | 0.000 s | 37.662 s |
| Churn benchmark overhead | 1.376 s | 1.170 s | 1.382 s | 1.169 s | 1.305 s | 1.166 s |
| Churn unaccounted time | 0.003 s | 0.003 s | 0.003 s | 0.004 s | 0.003 s | 0.004 s |
| Churn write 10-20 KiB | 64.3 KiB/s | 151.6 KiB/s | 43.7 KiB/s | 116.0 KiB/s | 41.4 KiB/s | 93.8 KiB/s |
| Churn write new 10-20 KiB | 64.2 KiB/s | 153.2 KiB/s | 43.6 KiB/s | 116.2 KiB/s | 41.3 KiB/s | 93.8 KiB/s |
| Churn write replace 10-20 KiB | 64.8 KiB/s | 147.2 KiB/s | 44.2 KiB/s | 115.4 KiB/s | 41.8 KiB/s | 93.7 KiB/s |
| Churn write 20-60 KiB | 88.9 KiB/s | 165.4 KiB/s | 64.9 KiB/s | 130.0 KiB/s | 58.8 KiB/s | 104.5 KiB/s |
| Churn write new 20-60 KiB | 90.6 KiB/s | 164.7 KiB/s | 66.1 KiB/s | 126.6 KiB/s | 59.6 KiB/s | 102.4 KiB/s |
| Churn write replace 20-60 KiB | 81.5 KiB/s | 168.8 KiB/s | 59.8 KiB/s | 148.6 KiB/s | 55.2 KiB/s | 115.4 KiB/s |
| Churn write 350 KiB | 59.1 KiB/s | 123.0 KiB/s | 40.9 KiB/s | 97.6 KiB/s | 38.4 KiB/s | 82.4 KiB/s |
| Churn delete avg | 9.39 ms | 9.88 ms | 9.35 ms | 9.66 ms | 10.6 ms | 10.6 ms |
| Churn delete p50 | 3.73 ms | 4.24 ms | 3.72 ms | 4.00 ms | 6.16 ms | 5.34 ms |
| Churn delete p95 | 32.5 ms | 40.3 ms | 32.5 ms | 41.5 ms | 32.7 ms | 37.9 ms |
| Churn delete max | 42.0 ms | 41.4 ms | 41.9 ms | 42.1 ms | 39.3 ms | 42.2 ms |
| Churn GC steps | 0 | 3,312 | 0 | 3,312 | 0 | 3,312 |
| Churn GC erased sectors | 0 | 1,164 | 0 | 1,110 | 0 | 1,110 |
| Smallfiles result | completed | completed | completed | completed | completed | completed |
| Smallfiles ops | 3244 | 3244 | 3244 | 3244 | 3244 | 3244 |
| Smallfiles bytes written | 8,389,152 | 8,389,152 | 8,389,152 | 8,389,152 | 8,389,152 | 8,389,152 |
| Smallfiles final live bytes | 2,335,027 | 2,335,027 | 2,335,027 | 2,335,027 | 2,335,027 | 2,335,027 |
| Smallfiles final live files | 922 | 922 | 922 | 922 | 922 | 922 |
| Smallfiles creates | 2446 | 2446 | 2446 | 2446 | 2446 | 2446 |
| Smallfiles replaces | 798 | 798 | 798 | 798 | 798 | 798 |
| Smallfiles deletes | 1524 | 1524 | 1524 | 1524 | 1524 | 1524 |
| Smallfiles written target | 8,388,608 | 8,388,608 | 8,388,608 | 8,388,608 | 8,388,608 | 8,388,608 |
| Smallfiles live target | 2,308,848 | 2,308,848 | 2,308,848 | 2,308,848 | 2,308,848 | 2,308,848 |
| Smallfiles file slots | 5,000 | 5,000 | 5,000 | 5,000 | 5,000 | 5,000 |
| Smallfiles total wall time | 202.719 s | 202.509 s | 508.909 s | 506.242 s | 1326.963 s | 1355.535 s |
| Smallfiles accounted time | 202.666 s | 202.451 s | 508.856 s | 506.182 s | 1326.877 s | 1355.448 s |
| Smallfiles write time | 192.288 s | 164.663 s | 498.739 s | 469.528 s | 1254.092 s | 1218.012 s |
| Smallfiles delete time | 8.518 s | 11.167 s | 8.247 s | 9.077 s | 70.993 s | 63.000 s |
| Smallfiles GC step time | 0.000 s | 24.769 s | 0.000 s | 25.747 s | 0.000 s | 72.658 s |
| Smallfiles benchmark overhead | 1.860 s | 1.852 s | 1.871 s | 1.830 s | 1.792 s | 1.778 s |
| Smallfiles unaccounted time | 0.053 s | 0.058 s | 0.053 s | 0.061 s | 0.086 s | 0.087 s |
| Smallfiles write 1-5120 B | 42.6 KiB/s | 49.8 KiB/s | 16.4 KiB/s | 17.4 KiB/s | 6.53 KiB/s | 6.73 KiB/s |
| Smallfiles write new 1-5120 B | 42.4 KiB/s | 49.8 KiB/s | 16.5 KiB/s | 17.5 KiB/s | 6.53 KiB/s | 6.70 KiB/s |
| Smallfiles write replace 1-5120 B | 43.2 KiB/s | 49.7 KiB/s | 16.1 KiB/s | 17.4 KiB/s | 6.54 KiB/s | 6.82 KiB/s |
| Smallfiles delete avg | 5.59 ms | 7.33 ms | 5.41 ms | 5.96 ms | 46.6 ms | 41.3 ms |
| Smallfiles delete p50 | 880 us | 1.08 ms | 874 us | 1.07 ms | 8.12 ms | 11.8 ms |
| Smallfiles delete p95 | 1.21 ms | 10.5 ms | 1.20 ms | 10.2 ms | 18.6 ms | 19.2 ms |
| Smallfiles delete max | 535 ms | 534 ms | 536 ms | 536 ms | 4.201 s | 4.217 s |
| Smallfiles GC steps | 0 | 53,024 | 0 | 54,938 | 0 | 54,938 |
| Smallfiles GC erased sectors | 0 | 819 | 0 | 603 | 0 | 603 |
| Smallfiles final list | 380 us | 381 us | 356 us | 364 us | 281 us | 258 us |
| Smallfiles cold mount | 5.52 ms | 5.53 ms | 5.48 ms | 4.37 ms | 5.99 ms | 6.19 ms |
| Smallfiles cold list | 138 ms | 138 ms | 138 ms | 137 ms | 1.636 s | 1.457 s |
| Smallfiles cold read 1-5120 B total | 2,412 KiB/s | 2,406 KiB/s | 2,414 KiB/s | 2,408 KiB/s | 291.5 KiB/s | 320.7 KiB/s |
| Smallfiles cold read 1-5120 B open/file | 279 us | 280 us | 279 us | 281 us | 6.74 ms | 6.07 ms |
| Smallfiles cold read 1-5120 B after open | 4,742 KiB/s | 4,736 KiB/s | 4,746 KiB/s | 4,753 KiB/s | 4,513 KiB/s | 4,517 KiB/s |
| Smallfiles cold exists existing avg | 156 us | 157 us | 155 us | 156 us | 6.46 ms | 5.82 ms |
| Smallfiles cold exists missing avg | 470 us | 467 us | 469 us | 469 us | 11.6 ms | 10.7 ms |
| Churn final list | 15.5 ms | 15.5 ms | 15.4 ms | 15.5 ms | 93.9 ms | 94.0 ms |
| Churn cold mount | 6.74 ms | 6.80 ms | 6.76 ms | 6.74 ms | 7.81 ms | 7.81 ms |
| Churn final cold list | 15.4 ms | 15.5 ms | 15.4 ms | 15.4 ms | 93.9 ms | 93.9 ms |
| Churn cold read 10-20 KiB total | 4,110 KiB/s | 4,102 KiB/s | 4,110 KiB/s | 4,109 KiB/s | 3,012 KiB/s | 3,010 KiB/s |
| Churn cold read 10-20 KiB open/file | 275 us | 277 us | 276 us | 275 us | 1.68 ms | 1.69 ms |
| Churn cold read 10-20 KiB after open | 4,646 KiB/s | 4,640 KiB/s | 4,647 KiB/s | 4,646 KiB/s | 4,631 KiB/s | 4,632 KiB/s |
| Churn cold read 20-60 KiB total | 4,344 KiB/s | 4,339 KiB/s | 4,346 KiB/s | 4,343 KiB/s | 3,698 KiB/s | 3,698 KiB/s |
| Churn cold read 20-60 KiB open/file | 273 us | 274 us | 273 us | 273 us | 1.80 ms | 1.80 ms |
| Churn cold read 20-60 KiB after open | 4,595 KiB/s | 4,591 KiB/s | 4,598 KiB/s | 4,594 KiB/s | 4,602 KiB/s | 4,603 KiB/s |
| Churn cold read 350 KiB total | 4,522 KiB/s | 4,517 KiB/s | 4,525 KiB/s | 4,525 KiB/s | 4,528 KiB/s | 4,528 KiB/s |
| Churn cold read 350 KiB open/file | 282 us | 274 us | 283 us | 274 us | 308 us | 308 us |
| Churn cold read 350 KiB after open | 4,575 KiB/s | 4,571 KiB/s | 4,579 KiB/s | 4,578 KiB/s | 4,583 KiB/s | 4,583 KiB/s |
| Churn cold exists existing avg | 151 us | 151 us | 151 us | 150 us | 1.92 ms | 1.92 ms |
| Churn cold exists missing avg | 50 us | 50 us | 50 us | 50 us | 3.35 ms | 3.35 ms |
| FS base memory | 4,924 | 4,924 | 4,788 | 4,788 | 308 | 308 |
| FS open file memory | 360 | 360 | 360 | 360 | 168 | 168 |
| FS stack memory | 1,236 | 1,284 | 1,292 | 1,292 | 1,292 | 1,244 |

FASTFFS preloads a small amount of file data during open, so the tiny-file after-open read row is effectively served from RAM. Compare tiny reads primarily using total throughput and open/file time.

## Benchmark Takeaways


- Namespace representation matters more than the simple linear-vs-indexed distinction. A RAM index gives FASTFFS its best open/exists/list latency, but even the minimal configuration remains competitive because the on-flash namespace is a compact, sequential metadata log that is cheap to scan. Linear scans become painful when each candidate requires scattered metadata reads, sector-head walks, or larger filesystem metadata structures to find a file.
- Garbage collection is not something you want to occasionally stop the world for in the middle of a write. The GC tax has to be paid eventually; paying it while there are spare cycles is usually better than forcing a foreground write to do the work when the caller may not be expecting it. The debt-GC simulation benchmarks show that dedicating some idle cycles improves overall efficiency, using fewer cycles overall.
- Tiny-file packing is important for config/asset-heavy workloads. Sector-per-file layouts can be simple and RAM-efficient, but they waste a lot of flash when many files are tens of bytes to a few kilobytes.
- FASTFFS expands the set of configurable RAM accelerators available to the application. The index cache, allocation bitmap, scratch size, and file cache each target different costs, so a build can spend RAM on the bottlenecks that matter for its workload.
- Flash read bandwidth is rarely the limiting factor for these workloads after a file is open. Metadata topology, erase/reclaim behavior, and open/list path length dominate perceived performance.
- Filesystems that try to cover broad POSIX-style semantics and robust generalized usage patterns with one structure cannot optimize as directly for specific workloads.
