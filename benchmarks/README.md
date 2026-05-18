# FASTFFS Candidate Benchmarks

This directory contains ESP32-S3 benchmark harnesses used to compare FASTFFS
against small NOR-flash filesystem candidates.

Current hardware/software baseline:

- ESP32-S3
- ESP-IDF v6.0-beta2
- 8 MB built-in flash
- 4 MB benchmark data partition at offset `0x190000`

## Projects

### `esp32s3_jesfs`

Standalone ESP-IDF benchmark for JesFS.

- Uses `third_party/JesFs`.
- Mounts a data partition named `jesfs`.
- Uses a JesFS-specific harness in `main/main.c`.
- Runs the raw partition timing probes before filesystem tests.

### `esp32s3_littlefs`

ESP-IDF VFS benchmark for LittleFS.

- Uses `joltwallet/littlefs` through the ESP-IDF component manager.
- Mounts a data partition named `storage` at `/fs`.
- Uses `benchmarks/vfs_bench_common`.
- Generated `managed_components/`, `dependencies.lock`, and `sdkconfig` are
  local build artifacts.

### `esp32s3_spiffs`

ESP-IDF VFS benchmark for SPIFFS.

- Uses ESP-IDF's built-in SPIFFS component.
- Mounts a data partition named `storage` at `/fs`.
- Uses `benchmarks/vfs_bench_common`.
- `sdkconfig.defaults` pins the SPIFFS tuning used for current measurements.

## Shared VFS Workload

`vfs_bench_common` is used by the LittleFS and SPIFFS projects. It measures:

- initial mount
- format and mount
- filesystem info and storage overhead
- 192 tiny file writes and reads, 64 bytes each
- 16 medium file writes and reads, 50 KiB each
- directory listing
- early/middle/late tiny-file lookup behavior
- `stat()` probes for existing and missing names
- cold remount plus read/list probes
- churn workload writing about 8 MiB total while keeping about 2.2 MiB live
- write health counters for short writes, retry loops, flush failures, and close
  failures

The churn mix is mostly 10-20 KiB files, some 20-60 KiB files, and one forced
350 KiB file late in the run.

## Build And Run

Use `benchidf.sh` from the repo root. It activates ESP-IDF, owns the project and
build-directory arguments, keeps build trees under each benchmark project, and
captures monitor output under `benchmarks/results/`.

```sh
benchmarks/benchidf.sh --list
benchmarks/benchidf.sh fastffs-default build
benchmarks/benchidf.sh jesfs build
benchmarks/benchidf.sh littlefs build
benchmarks/benchidf.sh spiffs build
```

Flash and monitor one project at a time:

```sh
benchmarks/benchidf.sh fastffs-default flash monitor
benchmarks/benchidf.sh jesfs flash monitor
benchmarks/benchidf.sh littlefs flash monitor
benchmarks/benchidf.sh spiffs flash monitor
```

Use `--port` or `ESPPORT` for a different attached board:

```sh
benchmarks/benchidf.sh fastffs-debt --port /dev/cu.usbserial-10 flash monitor
```

The wrapper adds monitor exits for crashes and the benchmark completion line
unless those specific monitor options are already supplied. Timestamped logs are
named like `benchmarks/results/20260518_143210_fastffs_default.log`.

Build directories are named `build-<variant>` and are always placed under the
selected benchmark project, for example
`benchmarks/esp32s3_fastffs/build-fastffs-default`. Do not pass `idf.py -B` or
`idf.py -C` directly for benchmark runs.

## Output Files

Local benchmark outputs are intentionally not tracked:

- `benchmarks/*/build/`
- `benchmarks/*/sdkconfig`
- `benchmarks/*/sdkconfig.old`
- `benchmarks/*/dependencies.lock`
- `benchmarks/*/managed_components/`
- `benchmarks/results/*.log`

Keep reproducible configuration in `sdkconfig.defaults`, not in generated
`sdkconfig` files. Result logs can be kept under `benchmarks/results/` for local
comparison, but they are treated as run artifacts.
