#!/usr/bin/env python3
import csv
import math
import statistics
import sys
from pathlib import Path


def percentile(values, pct):
    if not values:
        return None
    if len(values) == 1:
        return values[0]
    rank = (len(values) - 1) * pct
    lo = math.floor(rank)
    hi = math.ceil(rank)
    if lo == hi:
        return values[lo]
    return values[lo] + (values[hi] - values[lo]) * (rank - lo)


def load_perf(path):
    metrics = {}
    with open(path, newline="") as fh:
        for row in csv.reader(fh):
            if len(row) < 3:
                continue
            value = row[0].strip()
            event = row[2].strip()
            if not value or value == "<not counted>":
                continue
            try:
                metrics[event] = float(value)
            except ValueError:
                continue
    return metrics


def summarize_phase1(path):
    rows = []
    with open(path, newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            rows.append(row)

    if "dequeue_monotonic_ns" in rows[0]:
        latencies = [
            int(row["dequeue_monotonic_ns"]) - int(row["enqueue_monotonic_ns"])
            for row in rows
        ]
        intervals = [
            int(rows[i]["dequeue_monotonic_ns"])
            - int(rows[i - 1]["dequeue_monotonic_ns"])
            for i in range(1, len(rows))
        ]
        timestamp_source = "V4L2 buffer timestamp + CLOCK_MONOTONIC"
    else:
        latencies = [
            int(row["read_end_monotonic_ns"]) - int(row["read_start_monotonic_ns"])
            for row in rows
        ]
        intervals = [
            int(rows[i]["read_end_monotonic_ns"])
            - int(rows[i - 1]["read_end_monotonic_ns"])
            for i in range(1, len(rows))
        ]
        timestamp_source = "CLOCK_MONOTONIC"

    return {
        "frames": len(rows),
        "median_ns": statistics.median(latencies),
        "p95_ns": percentile(sorted(latencies), 0.95),
        "p99_ns": percentile(sorted(latencies), 0.99),
        "interval_stddev_ns": statistics.pstdev(intervals) if intervals else 0.0,
        "timestamp_source": timestamp_source,
    }


def summarize_phase2(path):
    rows = []
    with open(path, newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            rows.append(row)

    latencies = [
        int(row["vpipe_dq_monotonic_ns"]) - int(row["vivid_dq_monotonic_ns"])
        for row in rows
    ]
    return {
        "frames": len(rows),
        "median_ns": statistics.median(latencies),
        "p95_ns": percentile(sorted(latencies), 0.95),
        "p99_ns": percentile(sorted(latencies), 0.99),
    }


def summarize_latency_csv(path, start_key, end_key):
    rows = []
    with open(path, newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            rows.append(row)

    latencies = [int(row[end_key]) - int(row[start_key]) for row in rows]
    return {
        "frames": len(rows),
        "median_ns": statistics.median(latencies),
        "p95_ns": percentile(sorted(latencies), 0.95),
        "p99_ns": percentile(sorted(latencies), 0.99),
    }


def summarize_phase_userptr(path):
    rows = []
    with open(path, newline="") as fh:
        reader = csv.DictReader(fh)
        required = {
            "buffer_index",
            "enqueue_monotonic_ns",
            "dequeue_monotonic_ns",
            "minor_faults_delta",
        }
        if reader.fieldnames is None or not required.issubset(reader.fieldnames):
            missing = sorted(required.difference(reader.fieldnames or []))
            raise SystemExit(f"{path}: missing required columns: {missing}")
        for idx, row in enumerate(reader):
            try:
                row["_buffer_index"] = int(row["buffer_index"])
                row["_enqueue_ns"] = int(row["enqueue_monotonic_ns"])
                row["_dequeue_ns"] = int(row["dequeue_monotonic_ns"])
                row["_faults"] = int(row["minor_faults_delta"])
            except (TypeError, ValueError) as exc:
                raise SystemExit(f"{path}: non-numeric value at row {idx}: {exc}")
            rows.append(row)

    if not rows:
        raise SystemExit(f"{path}: no data rows")

    latencies = [row["_dequeue_ns"] - row["_enqueue_ns"] for row in rows]
    # The harness primes one USERPTR buffer per queue slot before STREAMON.
    # Those first-touch costs show up once per distinct buffer index, so skip
    # one initial dequeue for each buffer seen in the capture.
    warmup_rows = len({row["_buffer_index"] for row in rows})
    steady_faults = [row["_faults"] for row in rows[warmup_rows:]]
    return {
        "frames": len(rows),
        "median_ns": statistics.median(latencies),
        "p95_ns": percentile(sorted(latencies), 0.95),
        "p99_ns": percentile(sorted(latencies), 0.99),
        "warmup_rows_skipped": min(warmup_rows, len(rows)),
        "minor_faults_per_qbuf_mean": (
            statistics.mean(steady_faults) if steady_faults else 0.0
        ),
        "minor_faults_per_qbuf_p95": (
            percentile(sorted(steady_faults), 0.95) if steady_faults else 0
        ),
        "minor_faults_per_qbuf_max": max(steady_faults) if steady_faults else 0,
    }


def main():
    if len(sys.argv) != 3:
        print(
            "usage: summarize-benchmark.py "
            "phase1|phase2|phase_m2m|phase_fixture|phase_userptr csv_path",
            file=sys.stderr,
        )
        return 1

    mode = sys.argv[1]
    csv_path = Path(sys.argv[2])
    perf_path = csv_path.with_suffix(".perf.csv")

    if mode == "phase1":
        summary = summarize_phase1(csv_path)
    elif mode == "phase2":
        summary = summarize_phase2(csv_path)
    elif mode == "phase_m2m":
        summary = summarize_latency_csv(
            csv_path, "vivid_dq_monotonic_ns", "vpipe_dq_monotonic_ns"
        )
    elif mode == "phase_fixture":
        summary = summarize_latency_csv(
            csv_path, "enqueue_monotonic_ns", "dequeue_monotonic_ns"
        )
    elif mode == "phase_userptr":
        summary = summarize_phase_userptr(csv_path)
    else:
        print(f"unsupported mode: {mode}", file=sys.stderr)
        return 1

    perf = load_perf(perf_path) if perf_path.exists() else {}
    frames = summary["frames"]
    if perf and frames:
        summary["context_switches_per_frame"] = (
            perf.get("context-switches", 0.0) / frames
        )
        summary["cache_references_per_frame"] = (
            perf.get("cache-references", 0.0) / frames
        )
        summary["cache_misses_per_frame"] = perf.get("cache-misses", 0.0) / frames
        summary["cycles_per_frame"] = perf.get("cycles", 0.0) / frames
        summary["instructions_per_frame"] = perf.get("instructions", 0.0) / frames

    for key, value in summary.items():
        print(f"{key}={value}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
