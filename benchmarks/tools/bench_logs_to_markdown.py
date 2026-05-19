#!/usr/bin/env python3
"""Generate a benchmark comparison Markdown table from ESP-IDF log files.

Usage:
    bench_logs_to_markdown.py LOG[:LABEL] [LOG[:LABEL] ...]

The parser reads the benchmark's explicit summary lines. For churn total time,
it prefers `churn accounting wall_us=...`; older logs without that line fall
back to adjacent ESP-IDF log timestamps from `churn target` to `churn summary`.
"""

from __future__ import annotations

import argparse
from datetime import datetime
import re
from pathlib import Path


ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
LOG_TS_RE = re.compile(r"I \((\d+)\) [^:]+: (.*)")
KV_RE = re.compile(r"([A-Za-z0-9_]+)=([^ ]+)")
RUN_TS_RE = re.compile(r"(\d{8})_(\d{6})")

SIZE_CLASSES = {
    "small_10_20k": "10-20 KiB",
    "medium_20_60k": "20-60 KiB",
    "large_350k": "350 KiB",
}


def clean_line(line: str) -> str:
    return ANSI_RE.sub("", line).replace("\r", "").rstrip()


def kvs(text: str) -> dict[str, str]:
    return {match.group(1): match.group(2) for match in KV_RE.finditer(text)}


def seconds(us: str | int | None) -> str:
    if us is None:
        return ""
    return f"{int(us) / 1_000_000:.3f} s"


def time_us(us: str | int | None) -> str:
    if us is None:
        return ""
    value = int(us)
    if value < 1_000:
        return f"{value:,} us"
    if value < 1_000_000:
        ms = value / 1_000
        if ms < 10:
            return f"{ms:.2f} ms"
        if ms < 100:
            return f"{ms:.1f} ms"
        return f"{ms:,.0f} ms"
    return seconds(value)


def throughput_kib_s(bytes_value: str | int | None,
                     us_value: str | int | None) -> str:
    if bytes_value is None or us_value is None:
        return ""
    us = int(us_value)
    if us <= 0:
        return ""
    value = (int(bytes_value) * 1_000_000) / (1024 * us)
    if value < 10:
        return f"{value:.2f} KiB/s"
    if value < 1_000:
        return f"{value:.1f} KiB/s"
    return f"{value:,.0f} KiB/s"


def kib_s(value: str | int | None) -> str:
    if value is None:
        return ""
    numeric = int(value)
    if numeric < 10:
        return f"{numeric}.00 KiB/s"
    if numeric < 1_000:
        return f"{numeric:.1f} KiB/s"
    return f"{numeric:,} KiB/s"


def integer(value: str | int | None) -> str:
    if value is None:
        return ""
    return f"{int(value):,}"


class ParsedLog:
    def __init__(self, path: Path, label: str) -> None:
        timestamp_match = RUN_TS_RE.search(path.name)
        timestamp = ""
        if timestamp_match:
            timestamp = datetime.strptime(
                "".join(timestamp_match.groups()), "%Y%m%d%H%M%S"
            ).strftime("%Y-%m-%d %H:%M:%S")
        self.path = path
        self.label = label
        self.data: dict[str, str] = {
            "Log": path.name,
        }
        if timestamp:
            self.data["Run timestamp"] = timestamp
        self._churn_target_ms: int | None = None
        self._churn_summary_ms: int | None = None
        self._seen_churn_cold_mount = False

    def setdefault(self, key: str, value: str) -> None:
        if value and key not in self.data:
            self.data[key] = value

    def set(self, key: str, value: str) -> None:
        if value:
            self.data[key] = value

    def parse_line(self, text: str) -> None:
        ts_match = LOG_TS_RE.search(text)
        if not ts_match:
            return
        ts_ms = int(ts_match.group(1))
        msg = ts_match.group(2)

        if "config index_cache_mode=" in msg:
            d = kvs(msg)
            self.set("Index cache mode", d.get("index_cache_mode", ""))
            self.set("Index entries", d.get("index_heads", ""))
            self.set("Scratch bytes", d.get("scratch", ""))
            self.set("File write buffer", d.get("file_write_buffer", ""))
            self.set("Allocation map mode", d.get("alloc_map_mode", ""))
            self.set("Allocation map words", d.get("alloc_map_words", ""))
            self.set("Churn GC policy", d.get("gc_policy", ""))
            return

        if "baseline format rc=" in msg:
            self.set("Baseline format", time_us(kvs(msg).get("time_us")))
            return
        if "baseline mount rc=" in msg:
            self.set("Baseline mount after format", time_us(kvs(msg).get("time_us")))
            return
        if msg.startswith("format rc="):
            self.setdefault("Baseline format", time_us(kvs(msg).get("time_us")))
            return
        if msg.startswith("mount normal rc="):
            self.setdefault("Baseline mount after format", time_us(kvs(msg).get("time_us")))
            return
        if "baseline empty fsinfo" in msg:
            d = kvs(msg)
            self.set("Reported usable capacity", integer(d.get("total")))
            return
        if "baseline info " in msg:
            d = kvs(msg)
            self.set("Reported usable capacity", integer(d.get("free")))
            return
        if "baseline overhead_base " in msg:
            d = kvs(msg)
            self.set("Reported usable capacity", integer(d.get("available")))
            return
        if "write tiny files=" in msg:
            d = kvs(msg)
            self.set("Write 192 x 64 B",
                     throughput_kib_s(d.get("bytes"), d.get("time_us")))
            return
        if "after tiny storage_" in msg:
            d = kvs(msg)
            self.set("Storage 192 x 64 B overhead/file",
                     integer(d.get("overhead_per_file")))
            return
        if "read tiny split files=" in msg:
            d = kvs(msg)
            self.set("Read 192 x 64 B total",
                     throughput_kib_s(d.get("bytes"), d.get("total_us")))
            self.set("Read 192 x 64 B open/file",
                     time_us(round(int(d["open_us"]) / int(d["files"]))) if d.get("files") and int(d["files"]) else "")
            self.set("Read 192 x 64 B after open",
                     throughput_kib_s(d.get("bytes"), d.get("read_us")))
            return
        if "read tiny early index " in msg:
            d = kvs(msg)
            self.set("Read 32 early-index x 64 B total",
                     throughput_kib_s(d.get("bytes"), d.get("time_us")))
            return
        if "read tiny middle index " in msg:
            d = kvs(msg)
            self.set("Read 32 middle-index x 64 B total",
                     throughput_kib_s(d.get("bytes"), d.get("time_us")))
            return
        if "read tiny late index " in msg:
            d = kvs(msg)
            self.set("Read 32 late-index x 64 B total",
                     throughput_kib_s(d.get("bytes"), d.get("time_us")))
            return
        if "write medium files=" in msg:
            d = kvs(msg)
            self.set("Write 16 x 50 KiB",
                     throughput_kib_s(d.get("bytes"), d.get("time_us")))
            return
        if "after medium storage_" in msg:
            d = kvs(msg)
            self.set("Storage 208 mixed files overhead/file",
                     integer(d.get("overhead_per_file")))
            return
        if "read medium split files=" in msg:
            d = kvs(msg)
            self.set("Read 16 x 50 KiB total",
                     throughput_kib_s(d.get("bytes"), d.get("total_us")))
            self.set("Read 16 x 50 KiB open/file",
                     time_us(round(int(d["open_us"]) / int(d["files"]))) if d.get("files") and int(d["files"]) else "")
            self.set("Read 16 x 50 KiB after open",
                     throughput_kib_s(d.get("bytes"), d.get("read_us")))
            return
        if msg.startswith("list: entries=208 "):
            self.set("List 208 files", time_us(kvs(msg).get("time_us")))
            return
        if msg.startswith("list: active=208 "):
            self.set("List 208 files", time_us(kvs(msg).get("time_us")))
            return
        if "exists baseline tiny existing" in msg:
            self.set("Baseline exists tiny existing avg",
                     time_us(kvs(msg).get("avg_us")))
            return
        if "exists baseline tiny missing" in msg:
            self.set("Baseline exists tiny missing avg",
                     time_us(kvs(msg).get("avg_us")))
            return
        if "exists baseline medium existing" in msg:
            self.set("Baseline exists medium existing avg",
                     time_us(kvs(msg).get("avg_us")))
            return
        if "exists baseline medium missing" in msg:
            self.set("Baseline exists medium missing avg",
                     time_us(kvs(msg).get("avg_us")))
            return

        if "churn target " in msg:
            self._churn_target_ms = ts_ms
            d = kvs(msg)
            self.set("Churn GC policy", d.get("gc_policy", ""))
            self.set("Churn written target", integer(d.get("written_target")))
            self.set("Churn live target", integer(d.get("fixed_live_bytes")))
            return
        if "churn summary " in msg:
            self._churn_summary_ms = ts_ms
            d = kvs(msg)
            self.set("Churn ops", d.get("ops", ""))
            self.set("Churn bytes written", integer(d.get("written")))
            self.set("Churn final live bytes", integer(d.get("live")))
            self.set("Churn creates", d.get("creates", ""))
            self.set("Churn replaces", d.get("replaces", ""))
            self.set("Churn deletes", d.get("deletes", ""))
            return
        if "churn accounting " in msg:
            d = kvs(msg)
            gc_us = d.get("gc_step_us", d.get("scheduled_gc_us"))
            self.set("Churn total wall time", seconds(d.get("wall_us")))
            self.set("Churn accounted time", seconds(d.get("accounted_us")))
            self.set("Churn write time", seconds(d.get("write_us")))
            self.set("Churn delete time", seconds(d.get("delete_us")))
            self.set("Churn GC step time", seconds(gc_us))
            self.set("Churn benchmark overhead",
                     seconds(d.get("benchmark_overhead_us",
                                   d.get("unaccounted_us"))))
            self.set("Churn unaccounted time", seconds(d.get("unaccounted_us")))
            return
        if "churn live files avg=" in msg:
            d = kvs(msg)
            self.set("Churn average live files", d.get("avg", ""))
            return
        if "churn write class=" in msg:
            d = kvs(msg)
            cls = SIZE_CLASSES.get(d.get("class", ""), d.get("class", ""))
            self.set(f"Churn write {cls}",
                     throughput_kib_s(d.get("bytes"), d.get("time_us")))
            return
        if "churn create write class=" in msg:
            d = kvs(msg)
            cls = SIZE_CLASSES.get(d.get("class", ""), d.get("class", ""))
            self.set(f"Churn write new {cls}",
                     throughput_kib_s(d.get("bytes"), d.get("time_us")))
            return
        if "churn replace write class=" in msg:
            d = kvs(msg)
            cls = SIZE_CLASSES.get(d.get("class", ""), d.get("class", ""))
            self.set(f"Churn write replace {cls}",
                     throughput_kib_s(d.get("bytes"), d.get("time_us")))
            return
        if "churn delete latency " in msg:
            d = kvs(msg)
            self.set("Churn delete avg", time_us(d.get("avg_us")))
            self.set("Churn delete p50", time_us(d.get("p50_us")))
            self.set("Churn delete p95", time_us(d.get("p95_us")))
            self.set("Churn delete max", time_us(d.get("max_us")))
            return
        if "churn total gc " in msg:
            d = kvs(msg)
            self.set("Churn GC steps", integer(d.get("steps")))
            self.set("Churn GC erased sectors", integer(d.get("erased")))
            if "Churn GC step time" not in self.data:
                self.set("Churn GC step time", seconds(d.get("time_us")))
            return
        if msg.startswith("list: entries=123 "):
            key = "Churn final cold list, 123 files" if self._seen_churn_cold_mount else "Churn final list, 123 files"
            self.setdefault(key, time_us(kvs(msg).get("time_us")))
            return
        if msg.startswith("list: active=123 "):
            key = "Churn final cold list, 123 files" if self._seen_churn_cold_mount else "Churn final list, 123 files"
            self.setdefault(key, time_us(kvs(msg).get("time_us")))
            return
        if "churn cold mount rc=" in msg:
            self._seen_churn_cold_mount = True
            self.set("Churn cold mount", time_us(kvs(msg).get("time_us")))
            return
        if "churn cold read split class=" in msg:
            d = kvs(msg)
            cls = SIZE_CLASSES.get(d.get("class", ""), d.get("class", ""))
            self.set(f"Churn cold read {cls} total",
                     throughput_kib_s(d.get("bytes"), d.get("total_us")))
            files = int(d.get("files", "0"))
            if files:
                self.set(f"Churn cold read {cls} open/file",
                         time_us(round(int(d["open_us"]) / files)))
            self.set(f"Churn cold read {cls} after open",
                     throughput_kib_s(d.get("bytes"), d.get("read_us")))
            return
        if "exists churn cold existing" in msg:
            self.set("Churn cold exists existing avg",
                     time_us(kvs(msg).get("avg_us")))
            return
        if "exists churn cold missing" in msg:
            self.set("Churn cold exists missing avg",
                     time_us(kvs(msg).get("avg_us")))
            return
        if msg.startswith("memory "):
            d = kvs(msg)
            if d.get("base_valid") == "1":
                self.set("FS base memory", integer(d.get("base_bytes")))
            if d.get("open_file_valid") == "1":
                self.set("FS open file memory", integer(d.get("open_file_bytes")))
            if d.get("fs_stack_valid") == "1":
                self.set("FS stack memory", integer(d.get("fs_stack_bytes")))
            return

    def finalize(self) -> None:
        if "Churn total wall time" not in self.data:
            if self._churn_target_ms is not None and self._churn_summary_ms is not None:
                elapsed_us = (self._churn_summary_ms - self._churn_target_ms) * 1000
                self.data["Churn total wall time"] = seconds(elapsed_us) + " (from log timestamps)"


def parse_log(spec: str) -> ParsedLog:
    path_text, sep, label = spec.partition(":")
    path = Path(path_text)
    parsed = ParsedLog(path, label if sep else path.stem)
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            parsed.parse_line(clean_line(raw))
    parsed.finalize()
    return parsed


ROWS = [
    "Log",
    "Run timestamp",
    "Index cache mode",
    "Index entries",
    "Allocation map mode",
    "Allocation map words",
    "Scratch bytes",
    "File write buffer",
    "Churn GC policy",
    "Baseline format",
    "Baseline mount after format",
    "Reported usable capacity",
    "Storage 192 x 64 B overhead/file",
    "Write 192 x 64 B",
    "Read 192 x 64 B total",
    "Read 192 x 64 B open/file",
    "Read 192 x 64 B after open",
    "Read 32 early-index x 64 B total",
    "Read 32 middle-index x 64 B total",
    "Read 32 late-index x 64 B total",
    "Storage 208 mixed files overhead/file",
    "Write 16 x 50 KiB",
    "Read 16 x 50 KiB total",
    "Read 16 x 50 KiB open/file",
    "Read 16 x 50 KiB after open",
    "List 208 files",
    "Baseline exists tiny existing avg",
    "Baseline exists tiny missing avg",
    "Baseline exists medium existing avg",
    "Baseline exists medium missing avg",
    "Churn ops",
    "Churn bytes written",
    "Churn final live bytes",
    "Churn creates",
    "Churn replaces",
    "Churn deletes",
    "Churn average live files",
    "Churn total wall time",
    "Churn accounted time",
    "Churn write time",
    "Churn delete time",
    "Churn GC step time",
    "Churn benchmark overhead",
    "Churn unaccounted time",
    "Churn write 10-20 KiB",
    "Churn write new 10-20 KiB",
    "Churn write replace 10-20 KiB",
    "Churn write 20-60 KiB",
    "Churn write new 20-60 KiB",
    "Churn write replace 20-60 KiB",
    "Churn write 350 KiB",
    "Churn delete avg",
    "Churn delete p50",
    "Churn delete p95",
    "Churn delete max",
    "Churn GC steps",
    "Churn GC erased sectors",
    "Churn final list, 123 files",
    "Churn cold mount",
    "Churn final cold list, 123 files",
    "Churn cold read 10-20 KiB total",
    "Churn cold read 10-20 KiB open/file",
    "Churn cold read 10-20 KiB after open",
    "Churn cold read 20-60 KiB total",
    "Churn cold read 20-60 KiB open/file",
    "Churn cold read 20-60 KiB after open",
    "Churn cold read 350 KiB total",
    "Churn cold read 350 KiB open/file",
    "Churn cold read 350 KiB after open",
    "Churn cold exists existing avg",
    "Churn cold exists missing avg",
    "FS base memory",
    "FS open file memory",
    "FS stack memory",
]


def emit_markdown(logs: list[ParsedLog]) -> str:
    header = ["Stat"] + [log.label for log in logs]
    lines = [
        "| " + " | ".join(header) + " |",
        "|---" + "|---:" * len(logs) + "|",
    ]
    for row in ROWS:
        values = [log.data.get(row, "") for log in logs]
        if row != "Log" and not any(values):
            continue
        lines.append("| " + " | ".join([row] + values) + " |")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", help="Write Markdown table to this file")
    parser.add_argument("logs", nargs="+", help="Log path, optionally LOG:LABEL")
    args = parser.parse_args()
    parsed_logs = [parse_log(spec) for spec in args.logs]
    markdown = emit_markdown(parsed_logs)
    if args.output:
        Path(args.output).write_text(markdown + "\n", encoding="utf-8")
    else:
        print(markdown)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
