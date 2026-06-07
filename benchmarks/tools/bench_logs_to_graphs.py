#!/usr/bin/env python3
"""Generate benchmark graphs from FASTFFS ESP-IDF benchmark logs.

Usage:
    bench_logs_to_graphs.py -o benchmarks/results/graphs LOG[:LABEL] ...

The parser is shared with bench_logs_to_markdown.py. Labels are shortened for
charts, so FASTFFS default debt-GC becomes "FFFS", FASTFFS minimal debt-GC
becomes "FFFS min", FATFS becomes "FAT", and so on.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import sys
import tempfile
from typing import NamedTuple


os.environ.setdefault(
    "MPLCONFIGDIR",
    str(Path(tempfile.gettempdir()) / "fastffs-matplotlib-cache"),
)
os.environ.setdefault(
    "XDG_CACHE_HOME",
    str(Path(tempfile.gettempdir()) / "fastffs-xdg-cache"),
)

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
from matplotlib.patches import Patch


sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_logs_to_markdown import parse_log  # noqa: E402


class Series(NamedTuple):
    label: str
    color: str
    hatch: str
    data: dict[str, str]


class Metric(NamedTuple):
    label: str
    row: str


PALETTE = {
    "FFFS": "#F05A24",      # fire orange
    "FFFS min": "#F05A24",  # same base color, hatch-separated
    "LittleFS": "#D9B56F",  # close to #E5C17E, slightly deeper
    "FAT": "#008B8B",       # Windows 95 teal, slightly brighter
    "JesFS": "#1D4E89",     # dark blue
    "SPIFFS": "#7B4AB8",    # purple
}

HATCHES = {
    "FFFS min": "///",
}

CHART_FIGSIZE = (8.0, 4.8)
GROUPED_FIGSIZE = CHART_FIGSIZE
SINGLE_FIGSIZE = CHART_FIGSIZE
STACKED_FIGSIZE = CHART_FIGSIZE

READ_METRICS = [
    Metric("10-20 KiB", "Churn cold read 10-20 KiB total"),
    Metric("20-60 KiB", "Churn cold read 20-60 KiB total"),
    Metric("350 KiB", "Churn cold read 350 KiB total"),
]

WRITE_METRICS = [
    Metric("10-20 KiB", "Churn write 10-20 KiB"),
    Metric("20-60 KiB", "Churn write 20-60 KiB"),
    Metric("350 KiB", "Churn write 350 KiB"),
]

EXISTS_METRICS = [
    Metric("Exists", "Churn cold exists existing avg"),
    Metric("Missing", "Churn cold exists missing avg"),
]

def short_label(label: str) -> str:
    text = label.lower()
    if "fastffs" in text or "fffs" in text:
        if "minimal" in text or "crippled" in text or "min" in text:
            return "FFFS min"
        return "FFFS"
    if "little" in text:
        return "LittleFS"
    if "fat" in text:
        return "FAT"
    if "jes" in text:
        return "JesFS"
    if "spiffs" in text:
        return "SPIFFS"
    return label


def parse_number(value: str | None) -> float | None:
    if not value:
        return None
    text = value.replace(",", "").strip()
    match = re.match(r"^(-?\d+(?:\.\d+)?)\s*([A-Za-z/]+)?", text)
    if not match:
        return None
    number = float(match.group(1))
    unit = match.group(2) or ""
    if unit == "us":
        return number / 1000.0
    if unit == "ms":
        return number
    if unit == "s":
        return number * 1000.0
    return number


def metric_value(series: Series, row: str, scale: float = 1.0) -> float | None:
    value = parse_number(series.data.get(row))
    if value is None:
        return None
    return value / scale


def value_label(value: float) -> str:
    if value < 1:
        return f"{value:.2f}"
    if value < 10:
        return f"{value:.1f}"
    if value < 100:
        return f"{value:.1f}"
    if value < 1_000:
        return f"{value:.0f}"
    return f"{value:,.0f}"


def int_value(series: Series, row: str) -> int | None:
    value = series.data.get(row, "")
    text = value.replace(",", "").strip()
    if not text or not re.fullmatch(r"\d+", text):
        return None
    return int(text)


def setup_style() -> None:
    plt.rcParams.update(
        {
            "figure.facecolor": "#FFFFFF",
            "axes.facecolor": "#FFFFFF",
            "axes.edgecolor": "#D9DEE7",
            "axes.labelcolor": "#1F2937",
            "axes.titlecolor": "#111827",
            "xtick.color": "#4B5563",
            "ytick.color": "#4B5563",
            "font.family": "Avenir Next",
            "font.size": 13,
            "axes.linewidth": 1.1,
            "axes.titleweight": "bold",
            "axes.titlesize": 21,
            "axes.titlepad": 12,
            "axes.labelsize": 15,
            "xtick.labelsize": 14,
            "ytick.labelsize": 14,
            "xtick.major.width": 1.1,
            "ytick.major.width": 1.1,
            "legend.frameon": False,
            "legend.fontsize": 13,
            "savefig.bbox": None,
            "savefig.pad_inches": 0.18,
        }
    )


def save_figure(fig: plt.Figure, output_dir: Path, stem: str, formats: list[str], dpi: int) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    for fmt in formats:
        fig.savefig(output_dir / f"{stem}.{fmt}", dpi=dpi, format=fmt)
    plt.close(fig)


def add_legend(ax: plt.Axes, series: list[Series]) -> None:
    handles = [
        Patch(facecolor=item.color, edgecolor="#20242A", linewidth=0.7, hatch=item.hatch, label=item.label)
        for item in series
    ]
    ax.legend(
        handles=handles,
        ncols=len(handles),
        loc="upper center",
        bbox_to_anchor=(0.5, -0.085),
        columnspacing=0.9,
        handlelength=1.2,
        handletextpad=0.35,
        borderaxespad=0.0,
    )


def grouped_bars(
    series: list[Series],
    metrics: list[Metric],
    *,
    title: str,
    ylabel: str,
    stem: str,
    output_dir: Path,
    formats: list[str],
    dpi: int,
    log_y: bool,
    value_scale: float = 1.0,
    show_labels: bool = False,
    label_rotation: int = 90,
    label_fontsize: int = 9,
) -> None:
    fig, ax = plt.subplots(figsize=GROUPED_FIGSIZE)
    fig.subplots_adjust(left=0.11, right=0.985, top=0.86, bottom=0.16)
    group_count = len(metrics)
    series_count = len(series)
    width = min(0.12, 0.78 / max(series_count, 1))
    group_gap = 1.0
    positives: list[float] = []

    for s_index, item in enumerate(series):
        xs: list[float] = []
        ys: list[float] = []
        offset = (s_index - (series_count - 1) / 2.0) * width
        for m_index, metric in enumerate(metrics):
            value = metric_value(item, metric.row, value_scale)
            if value is None or value <= 0:
                continue
            xs.append(m_index * group_gap + offset)
            ys.append(value)
            positives.append(value)
        bars = ax.bar(
            xs,
            ys,
            width=width * 0.92,
            color=item.color,
            edgecolor="#20242A",
            linewidth=0.55,
            hatch=item.hatch,
            alpha=0.95,
        )
        if show_labels:
            for bar, value in zip(bars, ys):
                ax.text(
                    bar.get_x() + bar.get_width() / 2.0,
                    value * (1.08 if log_y else 1.015),
                    value_label(value),
                    ha="center",
                    va="bottom",
                    fontsize=label_fontsize,
                    rotation=label_rotation,
                )

    ax.set_title(title)
    ax.set_ylabel(ylabel)
    ax.set_xticks([index * group_gap for index in range(group_count)])
    ax.set_xticklabels([metric.label for metric in metrics])
    if log_y and positives:
        ax.set_yscale("log")
        ax.set_ylim(max(min(positives) / 1.8, 0.001), max(positives) * (2.2 if show_labels else 1.8))
    elif positives:
        ax.set_ylim(0, max(positives) * (1.34 if show_labels else 1.16))
    ax.grid(axis="y", color="#E5E7EB", linewidth=0.8, which="major")
    ax.grid(axis="y", color="#F1F5F9", linewidth=0.5, which="minor")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    add_legend(ax, series)
    save_figure(fig, output_dir, stem, formats, dpi)


def single_metric_bars(
    series: list[Series],
    row: str,
    *,
    title: str,
    ylabel: str,
    stem: str,
    output_dir: Path,
    formats: list[str],
    dpi: int,
    value_scale: float = 1.0,
    log_y: bool = False,
    y_cap: float | None = None,
) -> None:
    fig, ax = plt.subplots(figsize=SINGLE_FIGSIZE)
    fig.subplots_adjust(left=0.12, right=0.985, top=0.86, bottom=0.14)
    labels: list[str] = []
    values: list[float] = []
    colors: list[str] = []
    hatches: list[str] = []
    for item in series:
        value = metric_value(item, row, value_scale)
        if value is None or value <= 0:
            continue
        labels.append(item.label)
        values.append(value)
        colors.append(item.color)
        hatches.append(item.hatch)

    xs = list(range(len(values)))
    bars = ax.bar(xs, values, color=colors, edgecolor="#20242A", linewidth=0.7)
    for bar, hatch in zip(bars, hatches):
        bar.set_hatch(hatch)
    ax.set_title(title)
    ax.set_ylabel(ylabel)
    ax.set_xticks(xs)
    ax.set_xticklabels(labels, rotation=0)
    if y_cap is not None and values:
        ax.set_xlim(-0.7, len(labels) - 0.3)
        ax.set_ylim(0, y_cap * 1.13)
    elif log_y and values:
        ax.set_yscale("log")
        ax.set_ylim(max(min(values) / 1.8, 0.001), max(values) * 1.8)
    else:
        ax.set_ylim(0, max(values) * 1.22 if values else 1)
    ax.grid(axis="y", color="#E5E7EB", linewidth=0.8, which="major")
    ax.grid(axis="y", color="#F1F5F9", linewidth=0.5, which="minor")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

    for x, value, bar in zip(xs, values, bars):
        if y_cap is not None and value > y_cap:
            left = bar.get_x() - 0.01
            right = bar.get_x() + bar.get_width() + 0.01
            top = y_cap * 1.13
            fade_bottom = y_cap * 0.84
            gradient = [[(1.0, 1.0, 1.0, alpha / 95.0)] for alpha in range(96)]
            ax.imshow(
                gradient,
                extent=(left, right, fade_bottom, top),
                origin="lower",
                aspect="auto",
                interpolation="bicubic",
                clip_on=False,
                zorder=6,
            )
            label_y = y_cap * 1.06
        else:
            label_y = value * (1.05 if log_y else 1.015)
        label = value_label(value)
        ax.text(x, label_y, label, ha="center", va="bottom", fontsize=12, zorder=8)

    save_figure(fig, output_dir, stem, formats, dpi)


def churn_wall_time(
    series: list[Series],
    *,
    output_dir: Path,
    formats: list[str],
    dpi: int,
) -> None:
    y_limit = 200.0
    fig, ax = plt.subplots(figsize=SINGLE_FIGSIZE)
    fig.subplots_adjust(left=0.12, right=0.985, top=0.86, bottom=0.14)

    labels: list[str] = []
    wall_seconds: list[float] = []
    gc_seconds: list[float] = []
    colors: list[str] = []
    hatches: list[str] = []

    for item in series:
        wall = metric_value(item, "Churn total wall time", 1000.0)
        if wall is None or wall <= 0:
            continue
        gc = metric_value(item, "Churn GC step time", 1000.0) or 0.0
        gc = min(max(gc, 0.0), wall)
        labels.append(item.label)
        wall_seconds.append(wall)
        gc_seconds.append(gc)
        colors.append(item.color)
        hatches.append(item.hatch)

    xs = list(range(len(labels)))
    gc_bars = ax.bar(
        xs,
        gc_seconds,
        color="#475569",
        edgecolor="#20242A",
        linewidth=0.7,
    )
    for bar, gc in zip(gc_bars, gc_seconds):
        if gc <= 0:
            bar.set_alpha(0.0)

    non_gc_seconds = [wall - gc for wall, gc in zip(wall_seconds, gc_seconds)]
    bars = ax.bar(
        xs,
        non_gc_seconds,
        bottom=gc_seconds,
        color=colors,
        edgecolor="#20242A",
        linewidth=0.7,
    )
    for bar, hatch in zip(bars, hatches):
        bar.set_hatch(hatch)

    ax.set_title("Churn Wall Time")
    ax.set_ylabel("Seconds")
    ax.set_xticks(xs)
    ax.set_xticklabels(labels)
    ax.set_xlim(-0.7, len(labels) - 0.3)
    ax.set_ylim(0, y_limit * 1.13)
    ax.grid(axis="y", color="#E5E7EB", linewidth=0.8, which="major")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

    for x, wall, gc, bar in zip(xs, wall_seconds, gc_seconds, bars):
        if wall > y_limit:
            left = bar.get_x() - 0.01
            right = bar.get_x() + bar.get_width() + 0.01
            top = y_limit * 1.13
            fade_bottom = y_limit * 0.84
            gradient = [[(1.0, 1.0, 1.0, alpha / 95.0)] for alpha in range(96)]
            ax.imshow(
                gradient,
                extent=(left, right, fade_bottom, top),
                origin="lower",
                aspect="auto",
                interpolation="bicubic",
                clip_on=False,
                zorder=6,
            )
            label_y = y_limit * 1.06
        else:
            label_y = wall * 1.015
        ax.text(
            x,
            label_y,
            value_label(wall),
            ha="center",
            va="bottom",
            fontsize=12,
            zorder=8,
        )
        if gc > 0:
            ax.text(
                x,
                gc / 2.0,
                f"GC {value_label(gc)}",
                ha="center",
                va="center",
                fontsize=10,
                color="#FFFFFF",
                fontweight="bold",
            )

    ax.legend(
        handles=[
            Patch(facecolor="#475569", edgecolor="#20242A", linewidth=0.7, label="Background GC"),
        ],
        loc="upper center",
        bbox_to_anchor=(0.5, -0.085),
        ncols=1,
        columnspacing=1.0,
        handlelength=1.2,
        handletextpad=0.35,
        borderaxespad=0.0,
    )
    save_figure(fig, output_dir, "churn_wall_time", formats, dpi)


def smallfiles_extrapolated(
    series: list[Series],
    *,
    output_dir: Path,
    formats: list[str],
    dpi: int,
) -> None:
    fig, ax = plt.subplots(figsize=STACKED_FIGSIZE)
    fig.subplots_adjust(left=0.145, right=0.955, top=0.80, bottom=0.15)
    labels: list[str] = []
    actual_ms: list[float] = []
    remaining_ms: list[float] = []
    colors: list[str] = []
    hatches: list[str] = []
    notes: list[str] = []

    for item in series:
        wall = metric_value(item, "Smallfiles total wall time")
        written = int_value(item, "Smallfiles bytes written")
        target = int_value(item, "Smallfiles written target")
        if wall is None or written is None or target is None or written <= 0:
            continue
        estimated_total = wall * target / written
        remaining = max(0.0, estimated_total - wall)
        percent = min(100.0, 100.0 * written / target)
        result = item.data.get("Smallfiles result", "")

        labels.append(item.label)
        actual_ms.append(wall)
        remaining_ms.append(remaining)
        colors.append(item.color)
        hatches.append(item.hatch)
        if "failed" in result:
            notes.append(f"failed at {percent:.0f}%, est {estimated_total / 1000.0:.0f}s")
        else:
            notes.append(f"{wall / 1000.0:.0f}s")

    ys = list(range(len(labels)))
    actual_bars = ax.barh(
        ys,
        [value / 1000.0 for value in actual_ms],
        color=colors,
        edgecolor="#20242A",
        linewidth=0.7,
    )
    for bar, hatch in zip(actual_bars, hatches):
        bar.set_hatch(hatch)

    ax.barh(
        ys,
        [value / 1000.0 for value in remaining_ms],
        left=[value / 1000.0 for value in actual_ms],
        color="#E2E8F0",
        edgecolor="#64748B",
        linewidth=0.7,
        alpha=0.8,
    )

    totals = [(actual + remaining) / 1000.0 for actual, remaining in zip(actual_ms, remaining_ms)]
    for y, total, note in zip(ys, totals, notes):
        ax.text(total + max(totals) * 0.015, y, note, va="center", ha="left", fontsize=12, color="#374151")

    ax.set_title("Small Files Stress Test Wall Time", pad=16)
    ax.set_xlabel("Seconds, extrapolated to full write target")
    ax.set_yticks(ys)
    ax.set_yticklabels(labels)
    ax.invert_yaxis()
    ax.set_xlim(0, max(totals) * 1.22 if totals else 1)
    ax.grid(axis="x", color="#E5E7EB", linewidth=0.8)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(
        handles=[
            Patch(facecolor="#94A3B8", edgecolor="#20242A", linewidth=0.7, label="Observed"),
            Patch(
                facecolor="#E2E8F0",
                edgecolor="#64748B",
                linewidth=0.7,
                alpha=0.8,
                label="Estimated remaining",
            ),
        ],
        loc="lower right",
        bbox_to_anchor=(1.0, 1.12),
        ncols=2,
    )
    save_figure(fig, output_dir, "smallfiles_wall_time_extrapolated", formats, dpi)


def make_series(specs: list[str]) -> list[Series]:
    result: list[Series] = []
    used: set[str] = set()
    for spec in specs:
        parsed = parse_log(spec)
        label = short_label(parsed.label)
        if label in used:
            label = parsed.label
        used.add(label)
        result.append(
            Series(
                label=label,
                color=PALETTE.get(label, "#64748B"),
                hatch=HATCHES.get(label, ""),
                data=parsed.data,
            )
        )
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output-dir", default="benchmarks/results/graphs")
    parser.add_argument("--format", dest="formats", action="append", choices=["svg", "png", "pdf"], default=None)
    parser.add_argument("--dpi", type=int, default=135)
    parser.add_argument("logs", nargs="+", help="Log path, optionally LOG:LABEL")
    args = parser.parse_args()

    setup_style()
    output_dir = Path(args.output_dir)
    formats = args.formats or ["png"]
    series = make_series(args.logs)

    chart_count = 12
    single_metric_bars(
        series,
        "Read 192 x 64 B total",
        title="64 B File Read Throughput",
        ylabel="KiB/s",
        stem="read_64b_throughput",
        output_dir=output_dir,
        formats=formats,
        dpi=args.dpi,
    )
    single_metric_bars(
        series,
        "Write 192 x 64 B",
        title="64 B File Write Throughput",
        ylabel="KiB/s",
        stem="write_64b_throughput",
        output_dir=output_dir,
        formats=formats,
        dpi=args.dpi,
    )
    single_metric_bars(
        series,
        "Read 16 x 50 KiB total",
        title="50 KiB File Read Throughput",
        ylabel="KiB/s",
        stem="read_50k_throughput",
        output_dir=output_dir,
        formats=formats,
        dpi=args.dpi,
    )
    single_metric_bars(
        series,
        "Write 16 x 50 KiB",
        title="50 KiB File Write Throughput",
        ylabel="KiB/s",
        stem="write_50k_throughput",
        output_dir=output_dir,
        formats=formats,
        dpi=args.dpi,
    )
    grouped_bars(
        series,
        READ_METRICS,
        title="Churn Read Throughput By File Class",
        ylabel="KiB/s",
        stem="read_throughput_by_file_class",
        output_dir=output_dir,
        formats=formats,
        dpi=args.dpi,
        log_y=False,
    )
    grouped_bars(
        series,
        WRITE_METRICS,
        title="Churn Write Throughput By File Class",
        ylabel="KiB/s",
        stem="write_throughput_by_file_class",
        output_dir=output_dir,
        formats=formats,
        dpi=args.dpi,
        log_y=False,
    )
    grouped_bars(
        series,
        EXISTS_METRICS,
        title="Churn Exists / Missing Name Probe Time",
        ylabel="Milliseconds",
        stem="exists_time",
        output_dir=output_dir,
        formats=formats,
        dpi=args.dpi,
        log_y=True,
        show_labels=True,
        label_rotation=90,
        label_fontsize=14,
    )
    churn_wall_time(series, output_dir=output_dir, formats=formats, dpi=args.dpi)
    single_metric_bars(
        series,
        "Churn cold mount",
        title="Churn Mount Time",
        ylabel="Milliseconds",
        stem="mount_time",
        output_dir=output_dir,
        formats=formats,
        dpi=args.dpi,
        y_cap=100.0,
    )
    single_metric_bars(
        series,
        "Churn final cold list",
        title="Churn List Time",
        ylabel="Milliseconds",
        stem="list_time",
        output_dir=output_dir,
        formats=formats,
        dpi=args.dpi,
    )
    smallfiles_extrapolated(series, output_dir=output_dir, formats=formats, dpi=args.dpi)
    single_metric_bars(
        series,
        "Smallfiles write 1-5120 B",
        title="Small Files Stress Test Write Throughput",
        ylabel="KiB/s",
        stem="smallfiles_write_throughput",
        output_dir=output_dir,
        formats=formats,
        dpi=args.dpi,
    )
    print(f"wrote {len(formats) * chart_count} graph files to {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
