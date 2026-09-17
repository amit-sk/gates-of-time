import argparse
import csv
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import tempfile


SOURCE_FILES = ("task5.c", "got.c", "got.h", "consts.h", "arch_primitives.h")
INPUT_COMBINATIONS = (
    ("uncached", "uncached"),
    ("uncached", "cached"),
    ("cached", "uncached"),
    ("cached", "cached"),
)
DELAY_DEFINE = re.compile(
    r"^#define[ \t]+NAND2_MISPREDICTION_DELAY_STEPS[ \t]+\(?[ \t]*[0-9]+"
    r"[ \t]*\)?[ \t]*$",
    re.MULTILINE,
)
RESULT_BLOCK = re.compile(
    r"Inputs \((uncached|cached), (uncached|cached)\),[^\n]*:\n"
    r"(?:[^\n]*\n)*?[ \t]+Both correct: [0-9.]+% \(([0-9]+)/([0-9]+)\)",
)
SUMMARY_COLUMNS = (
    "steps",
    "runs",
    "uncached_uncached_mean_percent",
    "uncached_cached_mean_percent",
    "cached_uncached_mean_percent",
    "cached_cached_mean_percent",
    "minimum_mean_percent",
)


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Calibrate the NAND2 delay by rebuilding and running Task 5."
    )
    parser.add_argument("--min-steps", type=int, default=20)
    parser.add_argument("--max-steps", type=int, default=40)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--cpu", type=int, default=0)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()

    if not 0 <= arguments.min_steps <= arguments.max_steps:
        parser.error("steps must satisfy 0 <= min-steps <= max-steps")
    if arguments.runs < 1:
        parser.error("runs must be positive")
    if arguments.cpu < 0:
        parser.error("cpu must be nonnegative")
    if hasattr(os, "sched_getaffinity") and arguments.cpu not in os.sched_getaffinity(0):
        parser.error(f"CPU {arguments.cpu} is unavailable")
    return arguments


def configure_delay(working_directory, constants_text, delay_steps):
    updated_constants, replacement_count = DELAY_DEFINE.subn(
        f"#define NAND2_MISPREDICTION_DELAY_STEPS ({delay_steps})",
        constants_text,
        count=1,
    )
    if replacement_count != 1:
        raise ValueError("NAND2_MISPREDICTION_DELAY_STEPS must be defined once")
    (working_directory / "consts.h").write_text(updated_constants)


def compile_task5(working_directory):
    subprocess.run(
        [
            "cc",
            "-O1",
            "-falign-functions=8",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "task5.c",
            "got.c",
            "-o",
            "task5.out",
        ],
        cwd=working_directory,
        check=True,
        timeout=60,
    )
    return working_directory / "task5.out"


def run_task5(binary_path, cpu):
    completed = subprocess.run(
        ["taskset", "-c", str(cpu), str(binary_path)],
        check=True,
        capture_output=True,
        text=True,
        timeout=30,
    )
    accuracies = {}
    for input1, input2, correct_text, total_text in RESULT_BLOCK.findall(
        completed.stdout
    ):
        correct = int(correct_text)
        total = int(total_text)
        if total == 0 or not 0 <= correct <= total:
            raise ValueError(f"invalid result for ({input1}, {input2})")
        key = (input1, input2)
        if key in accuracies:
            raise ValueError(f"duplicate result for ({input1}, {input2})")
        accuracies[key] = 100.0 * correct / total

    if set(accuracies) != set(INPUT_COMBINATIONS):
        raise ValueError(f"unexpected Task 5 output:\n{completed.stdout}")
    return accuracies


def summarize_delay(delay_steps, run_results):
    summary = {"steps": delay_steps, "runs": len(run_results)}
    means = []
    for input1, input2 in INPUT_COMBINATIONS:
        mean = statistics.mean(result[(input1, input2)] for result in run_results)
        summary[f"{input1}_{input2}_mean_percent"] = mean
        means.append(mean)
    summary["minimum_mean_percent"] = min(means)
    return summary


def sweep_delays(source_directory, arguments):
    constants_text = (source_directory / "consts.h").read_text()
    summaries = []

    with tempfile.TemporaryDirectory(prefix="got-nand2-calibration-") as name:
        working_directory = Path(name)
        for filename in SOURCE_FILES:
            shutil.copy2(source_directory / filename, working_directory / filename)

        print(f"CPU {arguments.cpu}, {arguments.runs} runs per delay", flush=True)
        print("Steps       UU       UC       CU       CC    Minimum", flush=True)
        for delay_steps in range(arguments.min_steps, arguments.max_steps + 1):
            configure_delay(working_directory, constants_text, delay_steps)
            binary_path = compile_task5(working_directory)
            run_results = [
                run_task5(binary_path, arguments.cpu)
                for _ in range(arguments.runs)
            ]
            summary = summarize_delay(delay_steps, run_results)
            summaries.append(summary)
            print(
                f"{delay_steps:5d}"
                f" {summary['uncached_uncached_mean_percent']:8.2f}%"
                f" {summary['uncached_cached_mean_percent']:8.2f}%"
                f" {summary['cached_uncached_mean_percent']:8.2f}%"
                f" {summary['cached_cached_mean_percent']:8.2f}%"
                f" {summary['minimum_mean_percent']:9.2f}%",
                flush=True,
            )
    return summaries


def write_csv(output_path, summaries):
    output_path = output_path.resolve()
    with output_path.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=SUMMARY_COLUMNS)
        writer.writeheader()
        writer.writerows(summaries)
    print(f"Results: {output_path}")


def main():
    arguments = parse_arguments()
    source_directory = Path(__file__).resolve().parent
    summaries = sweep_delays(source_directory, arguments)
    best_result = max(
        summaries,
        key=lambda result: (
            result["minimum_mean_percent"],
            sum(
                result[f"{input1}_{input2}_mean_percent"]
                for input1, input2 in INPUT_COMBINATIONS
            ),
            -result["steps"],
        ),
    )

    print(f"\nBest NAND2 delay: {best_result['steps']} steps")
    print(
        f"#define NAND2_MISPREDICTION_DELAY_STEPS ({best_result['steps']})"
    )
    if arguments.output is not None:
        write_csv(arguments.output, summaries)


if __name__ == "__main__":
    main()
