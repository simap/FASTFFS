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

From the repo root, after activating ESP-IDF:

```sh
idf.py -C benchmarks/esp32s3_jesfs build
idf.py -C benchmarks/esp32s3_littlefs build
idf.py -C benchmarks/esp32s3_spiffs build
```

Flash and monitor one project at a time:

```sh
idf.py -C benchmarks/esp32s3_jesfs -p /dev/cu.usbserial-10 flash monitor
idf.py -C benchmarks/esp32s3_littlefs -p /dev/cu.usbserial-10 flash monitor
idf.py -C benchmarks/esp32s3_spiffs -p /dev/cu.usbserial-10 flash monitor
```

Use the serial port that matches the attached board.

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
