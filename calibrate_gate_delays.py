#!/usr/bin/env python3
"""Rebuild one delay at a time in copies of the existing task modules.

The repository's constants are never written by this script. Keep candidate
binaries, disassembly, exact stdout, run order, and observed runtime addresses
in --directory. Rank by the weakest truth-table case; two-output gates require
both outputs to be correct in the same trial.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shutil
import statistics
import subprocess
import time


GATES = {
    "not": (3, "not", "NOT"),
    "nand": (4, "nand", "NAND"),
    "fan2": (6, "fan2_impl", "FAN2"),
    "and": (6, "and_gate_impl", "AND"),
    "or": (6, "or_gate_impl", "OR"),
    "nand2": (5, "nand2", "NAND2"),
}
FILES = ("Makefile", "got.c", "got.h", "consts.h", "arch_primitives.h",
         "task3.c", "task4.c", "task5.c", "task6.c")


def training_counts(constants):
    defines = {name: int(value) for name, value in re.findall(
        r"^#define ([A-Z0-9_]*PREDICTOR_TRAINING_RUNS)[ \t]+\((\d+)\)[ \t]*$",
        constants, re.M)}
    default = defines["PREDICTOR_TRAINING_RUNS"]
    return {gate: defines.get(macro + "_PREDICTOR_TRAINING_RUNS", default)
            for gate, (_, _, macro) in GATES.items()}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True, stderr=subprocess.STDOUT)


def parse_output(gate, output):
    if gate == "not":
        part = output.split("not:\n", 1)[1].split("imul_not:", 1)[0]
        values = {}
        for state, correct, total in re.findall(
                r"(Cached|Uncached) input: .*?\((\d+)/(\d+) correct\)", part):
            values[state.lower()] = 100 * int(correct) / int(total)
        if len(values) != 2:
            raise ValueError("missing NOT cases")
        return {"cases": values, "details": values}

    if gate in ("fan2", "and", "or"):
        start, end = {
            "fan2": ("FAN2 gate:\n", "\nAND gate:"),
            "and": ("AND gate:\n", "\nOR gate:"),
            "or": ("OR gate:\n", "\nHalf adder:"),
        }[gate]
        output = output.split(start, 1)[1].split(end, 1)[0]

    if gate in ("nand", "and", "or"):
        values = {}
        for a, b, correct, total in re.findall(
                r"Inputs \((uncached|cached), (uncached|cached)\),[^\n]*?"
                r"\((\d+)/(\d+)(?: correct)?\)", output):
            values[a[0] + b[0]] = 100 * int(correct) / int(total)
        if len(values) != 4:
            raise ValueError("missing binary gate cases")
        return {"cases": values, "details": values}

    headers = (r"(Uncached|Cached) input, expected outputs (?:un)?cached:\n"
               if gate == "fan2" else
               r"Inputs \((uncached|cached), (uncached|cached)\), expected outputs (?:un)?cached:\n")
    matches = list(re.finditer(headers, output))
    cases, details = {}, {}
    for index, match in enumerate(matches):
        key = (match[1].lower() if gate == "fan2" else match[1][0] + match[2][0])
        block = output[match.end():matches[index + 1].start() if index + 1 < len(matches) else len(output)]
        rates = {label: 100 * int(correct) / int(total)
                 for label, correct, total in re.findall(
                     r"(Output 1 correct|Output 2 correct|Both correct): .*?\((\d+)/(\d+)\)", block)}
        if len(rates) != 3:
            raise ValueError("missing dual-output rates")
        cases[key] = rates["Both correct"]
        details[key] = rates
    if len(cases) != (2 if gate == "fan2" else 4):
        raise ValueError("missing dual-output cases")
    return {"cases": cases, "details": details}


def audit(binary, directory):
    nm = command(["nm", "-an", str(binary)])
    symbols = {name: int(address, 16) for address, kind, name in
               re.findall(r"^([0-9a-f]+) ([Tt]) (\S+)$", nm, re.M)}
    result = {"sha256": digest(binary), "size": binary.stat().st_size, "gates": {}}
    for _, symbol, _ in GATES.values():
        assembly = command(["objdump", "-d", "-Mintel", "--disassemble=" + symbol, str(binary)])
        (directory / (symbol + ".asm")).write_text(assembly)
        instructions = re.findall(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2}\s+)+\s*(\S+)\s*(.*?)$", assembly, re.M)
        result["gates"][symbol] = {
            "entry": symbols[symbol],
            "branches": [{"address": int(addr, 16), "mnemonic": op, "target": operand}
                         for addr, op, operand in instructions if op.startswith("j")],
            "instruction_count": len(instructions),
        }
    (directory / "symbols.txt").write_text(nm)
    return result


def build(root, directory, gate, steps, measure_gate=None):
    task, symbol, macro = GATES[gate]
    if measure_gate:
        task = GATES[measure_gate][0]
    directory.mkdir(parents=True, exist_ok=True)
    for filename in FILES:
        shutil.copy2(root / filename, directory / filename)
    header = (directory / "consts.h").read_text()
    header, count = re.subn(r"(#define " + macro + r"_MISPREDICTION_DELAY_STEPS )\(\d+\)",
                            lambda match: match[1] + f"({steps})", header)
    if count != 1:
        raise ValueError("delay define must occur once")
    (directory / "consts.h").write_text(header)
    build_log = command(["make", "-B", f"task{task}.out"], directory)
    (directory / "build.log").write_text(build_log)
    binary = directory / f"task{task}.out"
    metadata = audit(binary, directory)
    (directory / "audit.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return binary, metadata


def run(binary, cpu):
    started = time.monotonic()
    process = subprocess.Popen(["taskset", "-c", str(cpu), "setarch", "x86_64", "-R", str(binary)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    base = None
    try:
        for _ in range(100):
            try:
                maps = Path(f"/proc/{process.pid}/maps").read_text()
            except FileNotFoundError:
                break
            for line in maps.splitlines():
                fields = line.split()
                if len(fields) >= 6 and fields[-1] == str(binary):
                    base = int(fields[0].split("-")[0], 16) - int(fields[2], 16)
                    break
            if base is not None:
                break
            time.sleep(0.005)
        stdout, stderr = process.communicate(timeout=180)
        if process.returncode:
            raise RuntimeError(f"task exited {process.returncode}: {stderr}")
        if base != 0x555555554000:
            raise RuntimeError(f"unexpected/missing runtime load base: {base}")
        return stdout, base, time.monotonic() - started
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()


def summarize(records):
    result = []
    for steps in sorted({record["steps"] for record in records}):
        sample = [record for record in records if record["steps"] == steps]
        cases = {case: statistics.mean(record["cases"][case] for record in sample)
                 for case in sample[0]["cases"]}
        worst = [min(record["cases"].values()) for record in sample]
        result.append({"steps": steps, "runs": len(sample), "case_means": cases,
                       "worst_case_mean": min(cases.values()),
                       "mean_accuracy": statistics.mean(cases.values()),
                       "run_worst_range": [min(worst), max(worst)],
                       "run_worst_sd": statistics.stdev(worst) if len(worst) > 1 else 0,
                       "entry": sample[0]["entry"]})
    return sorted(result, key=lambda row: (-row["worst_case_mean"], -row["mean_accuracy"], row["steps"]))


def compare(root, args):
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    result_path = directory / (args.phase + ".json")
    if result_path.exists():
        raise ValueError("comparison phase already exists")
    reference = args.compare_with.resolve()
    records = []
    state = {"reference": str(reference), "current": str(root), "records": records,
             "cpu": args.cpu, "trials_per_gate": 100000,
             "cache_hit_threshold": 125, "aslr_disabled": True,
             "current_constants": (root / "consts.h").read_text(),
             "reference_constants": (reference / "consts.h").read_text()}
    state["training_counts"] = {
        "original": training_counts(state["reference_constants"]),
        "calibrated": training_counts(state["current_constants"])}
    for task in (3, 4, 6, 5):
        task_gates = [gate for gate, entry in GATES.items() if entry[0] == task]
        candidates = {}
        for index, source in enumerate((reference, root)):
            binary = source / f"task{task}.out"
            audit_dir = directory / f"task{task}_{index}"
            audit_dir.mkdir(exist_ok=True)
            metadata = audit(binary, audit_dir)
            (audit_dir / "audit.json").write_text(json.dumps(metadata, indent=2) + "\n")
            candidates[index] = (binary, metadata)
        for round_number in range(1, args.runs + 1):
            for index in ((0, 1) if round_number % 2 else (1, 0)):
                binary, metadata = candidates[index]
                stdout, base, elapsed = run(binary, args.cpu)
                log = directory / f"task{task}_{round_number}_{index}.log"
                log.write_text(stdout)
                for gate in task_gates:
                    parsed = parse_output(gate, stdout)
                    entry = metadata["gates"][GATES[gate][1]]["entry"]
                    header = state["reference_constants" if index == 0 else "current_constants"]
                    delay_steps = int(re.search(
                        r"^#define " + GATES[gate][2] + r"_MISPREDICTION_DELAY_STEPS \((\d+)\)$",
                        header, re.M)[1])
                    records.append({"gate": gate, "condition_index": index, "delay_steps": delay_steps,
                                    "condition": "original" if index == 0 else "calibrated",
                                    "training_runs": state["training_counts"]["original" if index == 0 else "calibrated"][gate],
                                    "round": round_number, "elapsed": elapsed,
                                    "load_base": base, "entry": entry, "runtime_entry": base + entry,
                                    "binary_sha256": metadata["sha256"], "stdout": str(log), **parsed})
                    print(f"{gate} {'original' if index == 0 else 'calibrated'} "
                          f"round {round_number}: worst={min(parsed['cases'].values()):.2f}%", flush=True)
                state["summary"] = {}
                for gate in {row["gate"] for row in records}:
                    summaries = summarize([{**row, "steps": row["condition_index"]}
                                           for row in records if row["gate"] == gate])
                    for summary in summaries:
                        index = summary.pop("steps")
                        summary["condition"] = "original" if index == 0 else "calibrated"
                        summary["training_runs"] = state["training_counts"][summary["condition"]][gate]
                        header = state["reference_constants" if index == 0 else "current_constants"]
                        summary["delay_steps"] = int(re.search(
                            r"^#define " + GATES[gate][2] + r"_MISPREDICTION_DELAY_STEPS \((\d+)\)$",
                            header, re.M)[1])
                    state["summary"][gate] = summaries
                result_path.write_text(json.dumps(state, indent=2) + "\n")
    print("SUMMARY " + json.dumps(state["summary"]), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--gate", choices=GATES)
    mode.add_argument("--compare-with", type=Path, help="directory with original task binaries and consts.h")
    parser.add_argument("--measure-gate", choices=GATES,
                        help="measure another gate while varying --gate (layout regression check)")
    parser.add_argument("--steps", help="comma-separated nonnegative delays")
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--cpu", type=int, default=0)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--phase", default="screen")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    if args.runs < 1 or args.cpu not in os.sched_getaffinity(0):
        parser.error("runs must be positive and CPU must be available")
    if args.compare_with:
        compare(root, args)
        return
    if not args.steps:
        parser.error("--steps is required with --gate")
    steps = sorted(set(int(value) for value in args.steps.split(",")))
    if not steps or min(steps) < 0 or args.runs < 1:
        parser.error("steps must be nonnegative; runs must be positive")
    if args.cpu not in os.sched_getaffinity(0):
        parser.error("CPU is unavailable")
    args.directory = args.directory.resolve()
    args.directory.mkdir(parents=True, exist_ok=True)
    result_path = args.directory / (args.phase + ".json")
    if result_path.exists():
        parser.error("phase already exists; choose a new name")
    measured = args.measure_gate or args.gate
    task, symbol, _ = GATES[measured]
    macro = GATES[args.gate][2]
    constants = (root / "consts.h").read_text()
    current = int(re.search(r"^#define " + macro + r"_MISPREDICTION_DELAY_STEPS \((\d+)\)$",
                            constants, re.M)[1])
    counts = training_counts(constants)
    source_hashes = {name: digest(root / name) for name in FILES}
    _, baseline_metadata = build(root, args.directory / "baseline", args.gate, current, measured)
    if digest(root / f"task{task}.out") != baseline_metadata["sha256"]:
        raise RuntimeError("baseline rebuild differs from the repository task binary; rebuild it first")
    candidates = {}
    for delay in steps:
        binary, metadata = build(root, args.directory / f"steps_{delay}", args.gate, delay, measured)
        if delay == current and digest(root / f"task{task}.out") != metadata["sha256"]:
            raise RuntimeError("baseline rebuild differs from the repository task binary; rebuild it first")
        candidates[delay] = (binary, metadata)
        print(f"built {args.gate}={delay}: {symbol}@{metadata['gates'][symbol]['entry']:#x}", flush=True)
    records = []
    rng = random.Random(7102026)
    state = {"gate": measured, "varied_gate": args.gate, "phase": args.phase, "baseline_steps": current,
             "cpu": args.cpu, "training_runs": counts[measured], "training_counts": counts,
             "source_hashes": source_hashes,
             "baseline_sha256": baseline_metadata["sha256"],
             "constants": (root / "consts.h").read_text(), "records": records}
    for round_number in range(1, args.runs + 1):
        order = steps.copy()
        rng.shuffle(order)
        for delay in order:
            binary, metadata = candidates[delay]
            stdout, base, elapsed = run(binary, args.cpu)
            parsed = parse_output(measured, stdout)
            log = args.directory / f"{args.phase}_{round_number}_{delay}.log"
            log.write_text(stdout)
            entry = metadata["gates"][symbol]["entry"]
            records.append({"steps": delay, "round": round_number, "elapsed": elapsed,
                            "load_base": base, "entry": entry, "runtime_entry": base + entry,
                            "binary_sha256": metadata["sha256"], "stdout": str(log), **parsed})
            state["summary"] = summarize(records)
            result_path.write_text(json.dumps(state, indent=2) + "\n")
            print(f"{args.gate}={delay} measuring {measured} round {round_number}/{args.runs}: "
                  f"worst={min(parsed['cases'].values()):.2f}% "
                  f"cases=" + ",".join(f"{key}:{value:.2f}" for key, value in parsed["cases"].items()), flush=True)
    print("SUMMARY " + json.dumps(state["summary"]), flush=True)


if __name__ == "__main__":
    main()
