#!/usr/bin/env python3
"""Fit and evaluate cache-level latency thresholds from raw measurements."""

from __future__ import annotations

import argparse
import csv
import html
import math
import statistics
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


REQUIRED_COLUMNS = {
    "mode",
    "label",
    "expected_level",
    "working_set_bytes",
    "sample",
    "latency_ticks",
}
LABELS = ("l1", "l2", "llc", "memory")
EXPECTED_LEVEL = {"l1": 1, "l2": 2, "llc": 3, "memory": 0}
PREPARED_COLORS = {
    "l1": "#2563eb",
    "l2": "#16a34a",
    "llc": "#d97706",
    "memory": "#dc2626",
}


@dataclass(frozen=True)
class Measurement:
    mode: str
    label: str
    expected_level: int | None
    working_set_bytes: int
    sample: int
    latency_ticks: int


def parse_int(value: str | None, name: str, line_number: int, *, optional: bool = False) -> int | None:
    text = "" if value is None else value.strip()
    if optional and not text:
        return None
    try:
        return int(text)
    except ValueError as exc:
        raise ValueError(f"line {line_number}: {name} must be an integer, got {text!r}") from exc


def read_measurements(path: Path) -> list[Measurement]:
    rows: list[Measurement] = []
    with path.open("r", encoding="utf-8-sig", newline="") as input_file:
        reader = csv.DictReader(input_file)
        if reader.fieldnames is None:
            raise ValueError("input is empty or has no CSV header")
        missing = REQUIRED_COLUMNS.difference(reader.fieldnames)
        if missing:
            raise ValueError("missing required CSV column(s): " + ", ".join(sorted(missing)))

        for line_number, raw in enumerate(reader, start=2):
            mode = (raw["mode"] or "").strip().lower().replace("-", "_")
            label = (raw["label"] or "").strip().lower()
            expected_level = parse_int(
                raw["expected_level"], "expected_level", line_number, optional=True
            )
            working_set_bytes = parse_int(raw["working_set_bytes"], "working_set_bytes", line_number)
            sample = parse_int(raw["sample"], "sample", line_number)
            latency_ticks = parse_int(raw["latency_ticks"], "latency_ticks", line_number)
            assert working_set_bytes is not None
            assert sample is not None
            assert latency_ticks is not None

            if not mode:
                raise ValueError(f"line {line_number}: mode is empty")
            if working_set_bytes < 0:
                raise ValueError(f"line {line_number}: working_set_bytes cannot be negative")
            if sample < 0:
                raise ValueError(f"line {line_number}: sample cannot be negative")
            if latency_ticks < 0:
                raise ValueError(f"line {line_number}: latency_ticks cannot be negative")

            if mode == "prepared":
                if label not in EXPECTED_LEVEL:
                    choices = ", ".join(LABELS)
                    raise ValueError(
                        f"line {line_number}: prepared label must be one of {choices}; got {label!r}"
                    )
                expected = EXPECTED_LEVEL[label]
                if expected_level != expected:
                    raise ValueError(
                        f"line {line_number}: label {label!r} requires expected_level {expected}, "
                        f"got {expected_level!r}"
                    )

            rows.append(
                Measurement(
                    mode=mode,
                    label=label,
                    expected_level=expected_level,
                    working_set_bytes=working_set_bytes,
                    sample=sample,
                    latency_ticks=latency_ticks,
                )
            )

    if not rows:
        raise ValueError("input contains no measurement rows")
    return rows


def percentile(sorted_values: Sequence[int], percent: float) -> float:
    if not sorted_values:
        raise ValueError("cannot calculate a percentile of an empty sequence")
    if len(sorted_values) == 1:
        return float(sorted_values[0])
    position = (len(sorted_values) - 1) * percent / 100.0
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(sorted_values[lower])
    fraction = position - lower
    return sorted_values[lower] * (1.0 - fraction) + sorted_values[upper] * fraction


def describe(values: Iterable[int]) -> dict[str, float | int]:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("cannot describe an empty sequence")
    return {
        "n": len(ordered),
        "min": ordered[0],
        "p05": percentile(ordered, 5),
        "median": percentile(ordered, 50),
        "p95": percentile(ordered, 95),
        "p99": percentile(ordered, 99),
        "max": ordered[-1],
        "mean": statistics.fmean(ordered),
        "stdev": statistics.pstdev(ordered),
    }


def format_bytes(size: int) -> str:
    value = float(size)
    units = ("B", "KiB", "MiB", "GiB", "TiB")
    unit = units[0]
    for unit in units:
        if value < 1024.0 or unit == units[-1]:
            break
        value /= 1024.0
    if unit == "B":
        return f"{size} B"
    if value >= 10:
        return f"{value:.0f} {unit}"
    return f"{value:.1f} {unit}"


def print_descriptive_stats(rows: Sequence[Measurement]) -> None:
    prepared = [row for row in rows if row.mode == "prepared"]
    print("\nPrepared-state latency statistics (ticks, all samples)")
    print("label        n      mean     stdev   min     p05  median     p95     p99     max")
    for label in LABELS:
        values = [row.latency_ticks for row in prepared if row.label == label]
        if not values:
            continue
        stats = describe(values)
        print(
            f"{label:<8} {stats['n']:7d} {stats['mean']:9.2f} {stats['stdev']:9.2f} "
            f"{stats['min']:5d} {stats['p05']:7.1f} {stats['median']:7.1f} "
            f"{stats['p95']:7.1f} {stats['p99']:7.1f} {stats['max']:7d}"
        )

    working_sets: dict[int, list[int]] = defaultdict(list)
    for row in rows:
        if row.mode == "working_set":
            working_sets[row.working_set_bytes].append(row.latency_ticks)
    if working_sets:
        print("\nWorking-set latency statistics (ticks, all samples)")
        print("working set       n      mean     stdev   min     p05  median     p95     p99     max")
        for size in sorted(working_sets):
            stats = describe(working_sets[size])
            print(
                f"{format_bytes(size):>11} {stats['n']:7d} {stats['mean']:9.2f} "
                f"{stats['stdev']:9.2f} {stats['min']:5d} {stats['p05']:7.1f} "
                f"{stats['median']:7.1f} {stats['p95']:7.1f} {stats['p99']:7.1f} "
                f"{stats['max']:7d}"
            )


def require_prepared_labels(rows: Sequence[Measurement], split_name: str) -> None:
    counts = Counter(row.label for row in rows)
    missing = [label for label in LABELS if counts[label] == 0]
    if missing:
        raise ValueError(
            f"{split_name} split has no prepared samples for: {', '.join(missing)}"
        )


def fit_thresholds(training_rows: Sequence[Measurement]) -> tuple[tuple[int, int, int], int]:
    """Return three integer thresholds and the minimum training error.

    Empty predicted classes are allowed when a gap contains enough distinct
    integer thresholds. The primary objective is the number of correctly
    classified rows. If several solutions have the same error, the one with the
    largest total boundary gap is used.
    """

    require_prepared_labels(training_rows, "training")
    ticks = sorted({row.latency_ticks for row in training_rows})
    if len(ticks) < 4:
        raise ValueError("training data needs at least four distinct latency values")

    tick_index = {tick: index for index, tick in enumerate(ticks)}
    counts = [[0] * len(ticks) for _ in LABELS]
    label_index = {label: index for index, label in enumerate(LABELS)}
    for row in training_rows:
        counts[label_index[row.label]][tick_index[row.latency_ticks]] += 1

    prefix = [[0] * (len(ticks) + 1) for _ in LABELS]
    for label in range(len(LABELS)):
        for index, count in enumerate(counts[label]):
            prefix[label][index + 1] = prefix[label][index] + count

    # A boundary b means that b distinct observed tick values compare <= the
    # threshold. Its interval contains every integer threshold with that effect.
    # The final interval is unbounded; three slots are sufficient because there
    # are only three thresholds.
    boundary_width = [ticks[0]]
    boundary_width.extend(ticks[b] - ticks[b - 1] for b in range(1, len(ticks)))
    boundary_width.append(3)

    # State values are (correct rows so far, total gap width, boundary path).
    # The key's second item counts consecutive thresholds in the same gap.
    State = tuple[int, int, tuple[int, ...]]
    states: dict[tuple[int, int], State] = {}
    for boundary in range(len(ticks) + 1):
        if boundary_width[boundary] >= 1:
            states[(boundary, 1)] = (
                prefix[0][boundary],
                boundary_width[boundary],
                (boundary,),
            )

    # Add the L2 and LLC regions. A running transformed maximum handles every
    # earlier boundary in O(n); a separate transition handles an empty region.
    for label in (1, 2):
        next_states: dict[tuple[int, int], State] = {}
        running: tuple[int, int, tuple[int, ...]] | None = None
        for boundary in range(len(ticks) + 1):
            previous_boundary = boundary - 1
            if previous_boundary >= 0:
                for repeat in range(1, 4):
                    state = states.get((previous_boundary, repeat))
                    if state is None:
                        continue
                    candidate = (
                        state[0] - prefix[label][previous_boundary],
                        state[1],
                        state[2],
                    )
                    if running is None or candidate[:2] > running[:2]:
                        running = candidate

            if running is not None and boundary_width[boundary] >= 1:
                next_states[(boundary, 1)] = (
                    running[0] + prefix[label][boundary],
                    running[1] + boundary_width[boundary],
                    running[2] + (boundary,),
                )

            for repeat in range(1, 3):
                state = states.get((boundary, repeat))
                if state is None or repeat >= boundary_width[boundary]:
                    continue
                candidate = (
                    state[0],
                    state[1] + boundary_width[boundary],
                    state[2] + (boundary,),
                )
                key = (boundary, repeat + 1)
                current = next_states.get(key)
                if current is None or candidate[:2] > current[:2]:
                    next_states[key] = candidate
        states = next_states

    best_solution: tuple[int, int, tuple[int, ...]] | None = None
    for (boundary, _), state in states.items():
        candidate = (
            state[0] + prefix[3][-1] - prefix[3][boundary],
            state[1],
            state[2],
        )
        if best_solution is None or candidate[:2] > best_solution[:2]:
            best_solution = candidate
    assert best_solution is not None
    correct, _, boundaries = best_solution

    values: list[int] = []
    group_start = 0
    while group_start < len(boundaries):
        group_end = group_start + 1
        while group_end < len(boundaries) and boundaries[group_end] == boundaries[group_start]:
            group_end += 1
        boundary = boundaries[group_start]
        count = group_end - group_start
        if boundary == 0:
            low, high = 0, ticks[0] - 1
        elif boundary == len(ticks):
            low = ticks[-1]
            high = low + count - 1
        else:
            low, high = ticks[boundary - 1], ticks[boundary] - 1
        first = (low + high - count + 1) // 2
        values.extend(range(first, first + count))
        group_start = group_end

    thresholds = (values[0], values[1], values[2])
    if not thresholds[0] < thresholds[1] < thresholds[2]:
        raise AssertionError(f"fitted thresholds are not strictly ordered: {thresholds}")
    return thresholds, len(training_rows) - correct


def classify(latency_ticks: int, thresholds: Sequence[int]) -> str:
    if latency_ticks <= thresholds[0]:
        return "l1"
    if latency_ticks <= thresholds[1]:
        return "l2"
    if latency_ticks <= thresholds[2]:
        return "llc"
    return "memory"


def confusion_matrix(
    rows: Sequence[Measurement], thresholds: Sequence[int]
) -> tuple[list[list[int]], int]:
    indices = {label: index for index, label in enumerate(LABELS)}
    matrix = [[0] * len(LABELS) for _ in LABELS]
    correct = 0
    for row in rows:
        predicted = classify(row.latency_ticks, thresholds)
        matrix[indices[row.label]][indices[predicted]] += 1
        correct += predicted == row.label
    return matrix, correct


def print_confusion(
    title: str, rows: Sequence[Measurement], thresholds: Sequence[int]
) -> tuple[int, float]:
    matrix, correct = confusion_matrix(rows, thresholds)
    print(f"\n{title} confusion matrix (rows are actual; columns are predicted)")
    print(f"{'actual':<10}" + "".join(f"{label:>10}" for label in LABELS))
    for label, values in zip(LABELS, matrix):
        print(f"{label:<10}" + "".join(f"{value:10d}" for value in values))
    accuracy = correct / len(rows)
    print(f"Accuracy: {correct}/{len(rows)} ({accuracy:.2%}); errors: {len(rows) - correct}")
    return len(rows) - correct, accuracy


def write_summary(
    path: Path,
    input_path: Path,
    thresholds: Sequence[int],
    training_rows: Sequence[Measurement],
    validation_rows: Sequence[Measurement],
    training_errors: int,
    validation_errors: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = (
        "input_csv",
        "fit_samples",
        "validation_samples",
        "l1_max_ticks",
        "l2_max_ticks",
        "llc_max_ticks",
        "fit_misclassified",
        "fit_accuracy",
        "validation_misclassified",
        "validation_accuracy",
    )
    with path.open("w", encoding="utf-8", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=fields)
        writer.writeheader()
        writer.writerow(
            {
                "input_csv": str(input_path),
                "fit_samples": len(training_rows),
                "validation_samples": len(validation_rows),
                "l1_max_ticks": thresholds[0],
                "l2_max_ticks": thresholds[1],
                "llc_max_ticks": thresholds[2],
                "fit_misclassified": training_errors,
                "fit_accuracy": f"{1.0 - training_errors / len(training_rows):.6f}",
                "validation_misclassified": validation_errors,
                "validation_accuracy": f"{1.0 - validation_errors / len(validation_rows):.6f}",
            }
        )


def histogram_series(
    groups: Sequence[tuple[str, str, Sequence[int]]], bin_width: int, max_ticks: int
) -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    for name, color, values in groups:
        counts: Counter[int] = Counter()
        clipped = 0
        for value in values:
            if value <= max_ticks:
                counts[value // bin_width] += 1
            else:
                clipped += 1
        total = len(values)
        normalized = {index: count / total for index, count in counts.items()}
        result.append(
            {
                "name": name,
                "color": color,
                "counts": normalized,
                "total": total,
                "clipped": clipped,
            }
        )
    return result


def sparse_histogram_points(counts: dict[int, float], max_bin: int) -> list[tuple[int, float]]:
    """Include zero-valued neighbors without emitting every empty histogram bin."""
    nonzero = sorted(index for index in counts if index <= max_bin)
    if not nonzero:
        return []
    indices = set(nonzero)
    for index in nonzero:
        if index > 0:
            indices.add(index - 1)
        if index < max_bin:
            indices.add(index + 1)
    return [(index, counts.get(index, 0.0)) for index in sorted(indices)]


def svg_text(x: float, y: float, text_value: str, **attributes: object) -> str:
    attrs = " ".join(f'{name.replace("_", "-")}="{html.escape(str(value))}"' for name, value in attributes.items())
    return f'<text x="{x:.1f}" y="{y:.1f}" {attrs}>{html.escape(text_value)}</text>'


def render_histogram_panel(
    top: int,
    title: str,
    series: Sequence[dict[str, object]],
    bin_width: int,
    max_ticks: int,
    thresholds: Sequence[int] | None = None,
) -> tuple[list[str], int]:
    plot_x = 76
    plot_y = top + 48
    plot_width = 880
    plot_height = 245
    legend_x = 985
    panel_height = max(330, 82 + 21 * len(series))
    max_bin = max_ticks // bin_width
    peak = max(
        (value for item in series for value in item["counts"].values()),
        default=1.0,
    )
    if peak <= 0:
        peak = 1.0

    elements = [svg_text(plot_x, top + 24, title, font_size=18, font_weight="600")]
    elements.append(
        f'<rect x="{plot_x}" y="{plot_y}" width="{plot_width}" height="{plot_height}" '
        'fill="#ffffff" stroke="#94a3b8"/>'
    )

    for tick in range(6):
        x = plot_x + plot_width * tick / 5
        tick_value = max_ticks * tick / 5
        elements.append(
            f'<line x1="{x:.1f}" y1="{plot_y}" x2="{x:.1f}" y2="{plot_y + plot_height}" '
            'stroke="#e2e8f0"/>'
        )
        elements.append(
            svg_text(x, plot_y + plot_height + 20, f"{tick_value:.0f}", text_anchor="middle", font_size=11)
        )
    for tick in range(5):
        y = plot_y + plot_height * (1.0 - tick / 4)
        tick_value = peak * tick / 4
        elements.append(
            f'<line x1="{plot_x}" y1="{y:.1f}" x2="{plot_x + plot_width}" y2="{y:.1f}" '
            'stroke="#e2e8f0"/>'
        )
        elements.append(
            svg_text(plot_x - 9, y + 4, f"{tick_value:.3f}", text_anchor="end", font_size=10)
        )

    if thresholds is not None:
        threshold_names = ("L1/L2", "L2/LLC", "LLC/memory")
        for index, (name, threshold) in enumerate(zip(threshold_names, thresholds)):
            if threshold > max_ticks:
                continue
            x = plot_x + plot_width * threshold / max_ticks
            elements.append(
                f'<line x1="{x:.1f}" y1="{plot_y}" x2="{x:.1f}" y2="{plot_y + plot_height}" '
                'stroke="#334155" stroke-width="1.2" stroke-dasharray="5 4"/>'
            )
            elements.append(
                svg_text(
                    x + 4,
                    plot_y + 14 + index * 14,
                    f"{name}: {threshold}",
                    font_size=10,
                    fill="#0f172a",
                )
            )

    elements.append(
        svg_text(
            plot_x + plot_width / 2,
            plot_y + plot_height + 42,
            "load latency (ticks)",
            text_anchor="middle",
            font_size=12,
        )
    )
    elements.append(
        f'<text x="18" y="{plot_y + plot_height / 2:.1f}" font-size="12" text-anchor="middle" '
        f'transform="rotate(-90 18 {plot_y + plot_height / 2:.1f})">fraction of all samples per bin</text>'
    )

    if not series:
        elements.append(
            svg_text(
                plot_x + plot_width / 2,
                plot_y + plot_height / 2,
                "No rows for this mode",
                text_anchor="middle",
                font_size=15,
                fill="#64748b",
            )
        )

    for series_index, item in enumerate(series):
        counts = item["counts"]
        assert isinstance(counts, dict)
        points = sparse_histogram_points(counts, max_bin)
        coordinates: list[str] = []
        for bin_index, fraction in points:
            center_tick = min((bin_index + 0.5) * bin_width, max_ticks)
            x = plot_x + plot_width * center_tick / max_ticks
            y = plot_y + plot_height * (1.0 - fraction / peak)
            coordinates.append(f"{x:.2f},{y:.2f}")
        if coordinates:
            elements.append(
                f'<polyline points="{" ".join(coordinates)}" fill="none" '
                f'stroke="{item["color"]}" stroke-width="1.8" stroke-linejoin="round"/>'
            )

        legend_y = plot_y + 5 + series_index * 21
        elements.append(
            f'<line x1="{legend_x}" y1="{legend_y}" x2="{legend_x + 24}" y2="{legend_y}" '
            f'stroke="{item["color"]}" stroke-width="3"/>'
        )
        clipped = int(item["clipped"])
        legend = f'{item["name"]} (n={item["total"]}'
        if clipped:
            legend += f", {clipped} > limit"
        legend += ")"
        elements.append(svg_text(legend_x + 32, legend_y + 4, legend, font_size=11))

    elements.append(
        svg_text(
            legend_x,
            plot_y + max(1, len(series)) * 21 + 24,
            f"bin width: {bin_width} ticks",
            font_size=10,
            fill="#475569",
        )
    )
    return elements, panel_height


def working_set_color(index: int, total: int) -> str:
    if total <= 1:
        return "hsl(270 65% 43%)"
    hue = 265 - 245 * index / (total - 1)
    return f"hsl({hue:.0f} 68% 43%)"


def write_svg(
    path: Path,
    rows: Sequence[Measurement],
    bin_width: int,
    max_ticks: int,
    thresholds: Sequence[int],
) -> None:
    prepared_groups: list[tuple[str, str, Sequence[int]]] = []
    for label in LABELS:
        values = [
            row.latency_ticks
            for row in rows
            if row.mode == "prepared" and row.label == label
        ]
        if values:
            prepared_groups.append((label, PREPARED_COLORS[label], values))

    working_values: dict[int, list[int]] = defaultdict(list)
    for row in rows:
        if row.mode == "working_set":
            working_values[row.working_set_bytes].append(row.latency_ticks)
    working_sizes = sorted(working_values)
    working_groups = [
        (
            format_bytes(size),
            working_set_color(index, len(working_sizes)),
            working_values[size],
        )
        for index, size in enumerate(working_sizes)
    ]

    prepared_series = histogram_series(prepared_groups, bin_width, max_ticks)
    working_series = histogram_series(working_groups, bin_width, max_ticks)
    first_elements, first_height = render_histogram_panel(
        56,
        "Prepared cache-state measurements",
        prepared_series,
        bin_width,
        max_ticks,
        thresholds,
    )
    second_top = 56 + first_height + 28
    second_elements, second_height = render_histogram_panel(
        second_top,
        "Random working-set measurements",
        working_series,
        bin_width,
        max_ticks,
    )
    height = second_top + second_height + 30

    document = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="{height}" '
        f'viewBox="0 0 1280 {height}">',
        '<rect width="100%" height="100%" fill="#f8fafc"/>',
        svg_text(640, 31, "Cache-load latency distributions", text_anchor="middle", font_size=22, font_weight="700"),
        *first_elements,
        *second_elements,
        "</svg>",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(document) + "\n", encoding="utf-8")


def output_path(input_path: Path, suffix: str) -> Path:
    return input_path.with_name(input_path.stem + suffix)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Fit cache-level latency thresholds on even-numbered prepared samples, "
            "validate them on odd-numbered samples, and plot the raw distributions."
        )
    )
    parser.add_argument("input_csv", type=Path, help="raw cache-latency CSV")
    parser.add_argument(
        "--summary",
        type=Path,
        help="threshold summary CSV (default: INPUT_thresholds.csv)",
    )
    parser.add_argument(
        "--plot",
        type=Path,
        help="histogram SVG (default: INPUT_histograms.svg)",
    )
    parser.add_argument(
        "--bin-width",
        type=int,
        default=2,
        help="histogram bin width in ticks (default: 2)",
    )
    parser.add_argument(
        "--max-ticks",
        type=int,
        help=(
            "maximum latency shown in the SVG "
            "(default: 99.5th percentile rounded up to the bin width)"
        ),
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.bin_width <= 0:
        raise ValueError("--bin-width must be positive")
    if args.max_ticks is not None and args.max_ticks <= 0:
        raise ValueError("--max-ticks must be positive")

    rows = read_measurements(args.input_csv)
    prepared = [row for row in rows if row.mode == "prepared"]
    if not prepared:
        raise ValueError("input has no prepared rows")
    training = [row for row in prepared if row.sample % 2 == 0]
    validation = [row for row in prepared if row.sample % 2 == 1]
    require_prepared_labels(training, "training")
    require_prepared_labels(validation, "validation")

    thresholds, fitted_error = fit_thresholds(training)
    actual_training_matrix, actual_training_correct = confusion_matrix(training, thresholds)
    del actual_training_matrix
    training_errors = len(training) - actual_training_correct
    if training_errors != fitted_error:
        raise AssertionError(
            f"threshold optimizer reported {fitted_error} errors, observed {training_errors}"
        )

    print(f"Read {len(rows)} measurements from {args.input_csv}")
    print(f"Prepared split: {len(training)} fitting (even sample), {len(validation)} validation (odd sample)")
    print_descriptive_stats(rows)
    print("\nFitted thresholds")
    print(f"l1:     latency <= {thresholds[0]} ticks")
    print(f"l2:     {thresholds[0]} < latency <= {thresholds[1]} ticks")
    print(f"llc:    {thresholds[1]} < latency <= {thresholds[2]} ticks")
    print(f"memory: latency > {thresholds[2]} ticks")
    training_errors, _ = print_confusion("Fitting", training, thresholds)
    validation_errors, _ = print_confusion("Validation", validation, thresholds)

    summary_path = args.summary or output_path(args.input_csv, "_thresholds.csv")
    plot_path = args.plot or output_path(args.input_csv, "_histograms.svg")
    if args.max_ticks is not None:
        plot_max_ticks = args.max_ticks
    else:
        all_ticks = sorted(row.latency_ticks for row in rows)
        plot_max_ticks = max(
            args.bin_width,
            math.ceil(percentile(all_ticks, 99.5) / args.bin_width) * args.bin_width,
        )
    write_summary(
        summary_path,
        args.input_csv,
        thresholds,
        training,
        validation,
        training_errors,
        validation_errors,
    )
    write_svg(plot_path, rows, args.bin_width, plot_max_ticks, thresholds)
    print(f"\nWrote threshold summary to {summary_path}")
    print(f"Wrote normalized histograms to {plot_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
