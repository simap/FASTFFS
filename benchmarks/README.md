# FASTFFS Candidate Benchmarks

ESP32-S3 benchmark harnesses comparing FASTFFS against small NOR-flash filesystem candidates.

Current hardware/software baseline:

- ESP32-S3
- ESP-IDF v6.0-beta2
- 8 MB built-in flash
- 4 MB benchmark data partition at offset `0x190000`

## Benchmark Runner

`benchfs/` is the shared benchmark runner. It owns workload generation, timing, accounting, churn modeling, stats, and table-log output. Each project provides a filesystem adapter: format/mount/unmount, file operations, listing/existence checks, and optional fsinfo, GC stepping, and memory reporting.

The workload covers format/mount, tiny and medium file write/read (192 x 64 B, 16 x 50 KB), storage overhead, listing, early/middle/late lookup behavior, exists probes for existing and missing names, cold remount probes, a deterministic churn phase (about 8.2 MB written, 2.2-2.4 MB live), a small-file churn phase (1 B-5 KB files, 5,000 slots, 8 MB write target), and write health counters.

## Projects

- `esp32s3_fastffs`: FASTFFS. One project builds all FASTFFS configurations; `benchidf.sh --list` shows the variants (default, no-alloc-map, minimal, inline vs debt GC, raw stack probe).
- `esp32s3_jesfs`: JesFS from `third_party/JesFs`, on a data partition named `jesfs`.
- `esp32s3_littlefs`: LittleFS via the `joltwallet/littlefs` component. Uses the shared VFS adapter.
- `esp32s3_spiffs`: ESP-IDF's built-in SPIFFS. Uses the shared VFS adapter; `sdkconfig.defaults` pins the SPIFFS tuning used for current measurements.
- `esp32s3_fatfs`: FatFs over the ESP-IDF wear-levelling layer with 4 KB sectors. Uses the shared VFS adapter.

`vfs_bench_common/` is the shared VFS adapter used by the LittleFS, SPIFFS, and FATFS projects. Their data partitions are named `storage` and mount at `/fs`.

## Build And Run

Use `benchidf.sh` from the repo root. It activates ESP-IDF, owns the project and build-directory arguments, keeps build trees under each benchmark project, and captures monitor output under `benchmarks/results/`.

```sh
benchmarks/benchidf.sh --list
benchmarks/benchidf.sh fastffs-default build
benchmarks/benchidf.sh jesfs build
benchmarks/benchidf.sh littlefs build
benchmarks/benchidf.sh fatfs build
benchmarks/benchidf.sh spiffs build
```

Flash and monitor one project at a time:

```sh
benchmarks/benchidf.sh fastffs-default flash monitor
benchmarks/benchidf.sh jesfs flash monitor
benchmarks/benchidf.sh littlefs flash monitor
benchmarks/benchidf.sh fatfs flash monitor
benchmarks/benchidf.sh spiffs flash monitor
```

Use `--port` or `ESPPORT` for a different attached board:

```sh
benchmarks/benchidf.sh fastffs-debt --port /dev/cu.usbserial-10 flash monitor
```

The wrapper adds monitor exits for crashes and the benchmark completion line unless those specific monitor options are already supplied. Timestamped logs are named like `benchmarks/results/20260518_143210_fastffs_default.log`.

Build directories are named `build-<variant>` and are always placed under the selected benchmark project, for example `benchmarks/esp32s3_fastffs/build-fastffs-default`. Do not pass `idf.py -B` or `idf.py -C` directly for benchmark runs.

## Output Files

Local benchmark outputs are intentionally not tracked:

- `benchmarks/*/build-*/`
- `benchmarks/*/sdkconfig`
- `benchmarks/*/sdkconfig.old`
- `benchmarks/*/dependencies.lock`
- `benchmarks/*/managed_components/`
- `benchmarks/results/*.log`

Keep reproducible configuration in `sdkconfig.defaults`, not in generated `sdkconfig` files. Result logs can be kept under `benchmarks/results/` for local comparison, but they are treated as run artifacts.
