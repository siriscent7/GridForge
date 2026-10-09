#!/usr/bin/env python3
"""Check the benchmark v2 exports independently using Python's standard library."""
import argparse
import collections
import csv
import json
import math
from pathlib import Path
import statistics

parser = argparse.ArgumentParser()
parser.add_argument("directory", type=Path)
parser.add_argument("--expected-samples", type=int)
parser.add_argument("--allow-failed", action="store_true")
args = parser.parse_args()
summary = json.loads((args.directory / "summary.json").read_text())
assert summary["schema"] == "gridforge.benchmark.v2"
with (args.directory / "results.csv").open(newline="") as handle:
    rows = list(csv.DictReader(handle))
if args.expected_samples is not None:
    assert len(rows) == args.expected_samples, (len(rows), args.expected_samples)
cases = {case["case_id"]: case for case in summary["cases"]}
samples = {}
orders = set()
for row in rows:
    assert row["verified"] == "true"
    case = cases[row["case_id"]]
    assert row["backend"] == case["backend"] and row["mode"] == case["mode"]
    assert row["workload"] == case["workload"]
    count = int(row["element_count"])
    assert count == case["element_count"]
    assert int(row["element_additions"]) == count * (2 if row["workload"] == "dependency" else 1)
    key = (row["case_id"], int(row["iteration_id"]))
    assert key not in samples
    samples[key] = row
    order = int(row["measurement_order"])
    assert order > 0 and order not in orders
    orders.add(order)
    resident = row["workload"] == "resident"
    assert bool(row["resident_compute_ms"]) == resident
    assert bool(row["end_to_end_ms"]) != resident
    assert bool(row["submission_ms"]) == (row["mode"] == "async")
    for metric in ("end_to_end_ms", "submission_ms", "resident_compute_ms"):
        if row[metric]:
            assert math.isfinite(float(row[metric])) and float(row[metric]) >= 0
    if row["submission_ms"]:
        assert float(row["submission_ms"]) <= float(row["resident_compute_ms"] or row["end_to_end_ms"])

for case in cases.values():
    if not args.allow_failed:
        assert case["status"] == "verified", case["failure_reason"]
    case_rows = [row for row in rows if row["case_id"] == case["case_id"]]
    assert len(case_rows) == case["verified_sample_count"]
    if case["partitions"]:
        assert sum(case["partitions"]) == case["element_count"]
    for metric in ("end_to_end_ms", "submission_ms", "resident_compute_ms"):
        values = [float(row[metric]) for row in case_rows if row[metric]]
        stats = case["statistics"][metric]
        if not values:
            assert stats is None
            continue
        assert stats["count"] == len(values)
        expected = {"min": min(values), "max": max(values), "mean": statistics.mean(values),
                    "median": statistics.median(values),
                    "p95_nearest_rank": sorted(values)[math.ceil(.95 * len(values)) - 1]}
        if len(values) > 1:
            expected["sample_stddev"] = statistics.stdev(values)
        else:
            assert stats["sample_stddev"] is None
        for name, value in expected.items():
            assert math.isclose(stats[name], value, rel_tol=1e-10, abs_tol=1e-12), (case["case_id"], metric, name)

# Validate actual chronology, rather than trusting the summary's order description.
backend_names = [backend["backend"] for backend in summary["backends"]]
matrices = [[case for case in summary["cases"] if case["backend"] == name] for name in backend_names]
assert all(len(matrix) == len(matrices[0]) for matrix in matrices)
logical_indices = {case["case_id"]: index for matrix in matrices for index, case in enumerate(matrix)}
pairs = collections.defaultdict(list)
for row in rows:
    pairs[(logical_indices[row["case_id"]], int(row["iteration_id"]))].append(row)
for (index, iteration), paired in pairs.items():
    if any(matrix[index]["status"] != "verified" for matrix in matrices):
        continue
    paired.sort(key=lambda row: int(row["measurement_order"]))
    expected = [backend_names[(iteration - 1 + index + offset) % len(backend_names)]
                for offset in range(len(backend_names))]
    assert [row["backend"] for row in paired] == expected, (
        "Backend order did not alternate for the logical case", index, iteration, expected)
    first_order = int(paired[0]["measurement_order"])
    assert [int(row["measurement_order"]) for row in paired] == list(range(first_order, first_order + len(paired)))
    match_fields = ("workload", "mode", "element_count", "requested_stream_count", "worker_count", "seed")
    assert all(all(row[field] == paired[0][field] for field in match_fields) for row in paired)

instrumentation = summary["instrumentation"]
assert instrumentation["enabled"] == summary["instrumented"]
assert sum(case["dropped_metric_records"] for case in cases.values()) == instrumentation["dropped_records"]
assert sum(case["metric_records_written"] for case in cases.values()) == instrumentation["retained_records_written"]
groups = collections.Counter()
record_counts = collections.Counter()
identities = set()
record_count = 0
if instrumentation["enabled"]:
    assert instrumentation["complete"] == (instrumentation["dropped_records"] == 0)
    assert instrumentation["records_file"] == "operations.jsonl"
    with (args.directory / instrumentation["records_file"]).open() as handle:
        for line in handle:
            record = json.loads(line)
            assert record["schema"] == "gridforge.operation.v1"
            case = cases[record["case_id"]]
            assert case["mode"] == "async"
            operation = record["operation"]
            assert operation["backend"] == case["backend"]
            identity = (operation["backend"], operation["operation_id"])
            assert identity not in identities
            identities.add(identity)
            phase, iteration, order = record["phase"], record["iteration_id"], record["measurement_order"]
            assert phase in ("preparation", "warmup", "measured")
            if phase == "preparation":
                assert iteration == 0 and order == 0
            elif phase == "warmup":
                assert 1 <= iteration <= summary["warmup_count"] and order == 0
            else:
                assert 1 <= iteration <= summary["measured_iterations"] and order > 0
                if case["status"] == "verified":
                    assert int(samples[(case["case_id"], iteration)]["measurement_order"]) == order
            groups[(case["case_id"], phase, iteration)] += 1
            record_counts[case["case_id"]] += 1
            record_count += 1
            accepted, retired = operation["accepted"], operation["retirement_publication"]
            assert isinstance(accepted, int) and isinstance(retired, int) and accepted <= retired
            for name in ("execution_or_submission_start", "host_completion", "metal_commit_observed", "metal_completion_observed"):
                if operation[name] is not None:
                    assert accepted <= operation[name] <= retired
            if operation["terminal_status"] == "skipped":
                assert operation["execution_or_submission_start"] is None
            gpu = [operation[name] for name in ("metal_gpu_start_seconds", "metal_gpu_end_seconds", "gpu_command_buffer_execution_duration_seconds")]
            if gpu[0] is None:
                assert gpu == [None, None, None]
            else:
                start, end, duration = gpu
                assert operation["backend"] == "metal" and all(math.isfinite(value) for value in gpu)
                assert 0 < start <= end
                assert math.isclose(end - start, duration, rel_tol=1e-7, abs_tol=1e-10)
            if operation["backend"] == "cpu":
                assert gpu == [None, None, None]
    assert record_count == instrumentation["retained_records_written"]
    for case in cases.values():
        assert record_counts[case["case_id"]] == case["metric_records_written"]
        if case["mode"] != "async" or case["status"] != "verified" or case["dropped_metric_records"]:
            continue
        per_iteration = (9 if case["workload"] == "dependency" else 2 if case["workload"] == "resident"
                         else 4 * len(case["partitions"]))
        for phase, iterations in (("warmup", summary["warmup_count"]), ("measured", summary["measured_iterations"])):
            for iteration in range(1, iterations + 1):
                assert groups[(case["case_id"], phase, iteration)] == per_iteration, (case["case_id"], phase, iteration)
else:
    assert instrumentation["records_file"] is None and record_count == instrumentation["retained_records_written"] == 0
print(f"PASS: {len(rows)} verified samples across {len(cases)} cases; {record_count} attributed operations; {instrumentation['dropped_records']} drops")
