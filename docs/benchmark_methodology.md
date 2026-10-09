# GridForge benchmark methodology

GridForge benchmarks are measurement infrastructure, not performance claims. They use `std::chrono::steady_clock`; run them in a Release build. Optional asynchronous-operation instrumentation is available with `--detailed`.

## Prepared cases and workloads

For each backend/workload/size/stream-count/mode case, deterministic inputs and expected outputs are generated outside execution timing. The runtime is initialized once per backend; buffers and any async streams are created once per case and reused for warmups and measured iterations. Runtime initialization, per-case buffer allocation, and stream creation are reported separately.

`--mode sync|async|both` selects execution modes and defaults to `both`. Synchronous and asynchronous stream workloads use the same total element count and identical partition boundaries. Async independent-stream work is submitted to every stream before the runner waits on download results. The dependency pipeline computes the reference using two sequential float additions, `(A + B) + B`.

## Timing boundaries

All durations are in milliseconds. End-to-end timing starts immediately before the first upload/submission and ends after all requested download results are available. Async submission timing starts before the first async API call and ends when the last submission call returns; it is a host API duration, not GPU execution time. Resident-compute timing starts at the first launch and ends after final completion; input uploads happen before this interval and output download happens afterward. Verification and copying owned download results into verification storage are outside the execution intervals. Async result handles are released and accepted work is retired before the next iteration.

Inapplicable timings are represented as empty CSV fields and `null` in JSON. No single elapsed interval is copied into multiple timing categories.

## Samples, statistics, and ordering

`results.csv` contains one row for each verified measured iteration, identified by `case_id` and `iteration_id`. It includes actual stream count, launch count, and element-addition count. `summary.json` contains per-case sample count, min, median, mean, sample standard deviation, max, and nearest-rank p95 for each applicable execution timing. Failed cases and their reasons remain in the summary; failed/unverified iterations are not exported as successful raw samples or included in statistics.

Warmups run before measured rounds. Within each backend, measured case order is deterministically rotated by round to reduce fixed-order bias. The stream workload keeps fixed total work while partitioning elements as evenly as possible; for uneven divisions, the first remainder partitions receive one extra element.

When CPU and Metal are both available, matching case indices are paired and backends alternate in a deterministic order within each measured round; the logical case index also rotates by round. This avoids measuring all CPU cases before all Metal cases. A case is drained before the next backend/case measurement.

The first backend is selected from the measured round and the stable logical case index, independently of the rotating position. Every logical case has CPU first in ten rounds and Metal first in ten rounds for the default twenty-iteration run. Using the rotating offset here would cancel round parity and leave backend order fixed. The export checker validates actual sample chronology and rejects that previous ordering bug.

## Optional runtime operation metrics

Instrumentation is disabled unless `--detailed` is specified. Each runtime then keeps preallocated completed-record storage, with a default capacity of 4,096 records. `--metric-capacity N` can select a smaller diagnostic capacity or increase it up to 65,536. When full, new records are dropped. The runtime drain API returns a cumulative drop counter; the benchmark accounts for its increment at each drain, avoiding double counting.

After each case preparation, warmup, and measured iteration, the runner waits for retirement and drains that case's metrics. Each retained record is streamed to `operations.jsonl` outside execution timing, scheduler locks, and callbacks. No growing operation-record vector is retained by the benchmark. Drops and counts are attributed to each case; `instrumentation.complete` is false if any drops occurred. A long run can retain more than 4,096 total records because storage is reused between iterations.

The summary schema is `gridforge.benchmark.v2`. Embedded `instrumentation.records` from v1 is replaced by `instrumentation.records_file`, `record_schema`, and `retained_records_written`. Each JSONL line has schema `gridforge.operation.v1`, `case_id`, `phase`, `iteration_id`, `measurement_order`, and the existing runtime record under `operation`. Phases are `preparation`, `warmup`, or `measured`. Preparation uses iteration/order zero; warmups use one-based iteration and order zero. Measured operation records match the case/iteration/order fields in `results.csv`. Detailed synchronous-only cases emit no operations, consistent with the runtime's asynchronous coverage.

Each record reports operation kind, backend, stream ID, operation ID, and terminal status (`succeeded`, `failed`, or `skipped`). Host timestamps use nanoseconds from the `steady_clock` epoch and are nullable when inapplicable: successful acceptance; execution/submission start; host-operation completion; Metal commit observed after submit returns; Metal completion observed by the runtime callback; and retirement publication before event/result readiness is published. The submission-return/completion-received handshake remains in force even if a Metal callback wins the race and arrives first.

Runtime operation records cover accepted asynchronous stream operations, including event barriers/records. Synchronous `Buffer`/`Runtime` calls and submissions rejected before acceptance are not emitted as operation records.

After successful Metal command-buffer completion, the private bridge reads GPUStartTime and GPUEndTime. It exports them only when both are finite, positive, and ordered. Their difference is labeled GPU command-buffer execution duration in seconds. These GPU timestamps are separate from host `steady_clock` timestamps; no overlapping duration is summed into wall-clock time. Instrumented runs add timestamp and bounded-ring overhead and are diagnostic, not directly comparable to ordinary performance runs.

## Reproducibility metadata

The summary includes UTC run time, requested backends and machine-readable backend omissions, selected device names, partitions, work counts, workers, runtime allocation/staging/outstanding-operation limits, seed, warmup and measured iteration counts, instrumentation state/capacity/drop count, OS/release, architecture, compiler, build type, and Git revision plus dirty state when obtainable. It intentionally excludes usernames, serial numbers, credentials, and device UUIDs.

## Fairness and safety

- Resource estimates use checked arithmetic. Before any case inputs/buffers are created, the entire retained matrix must fit each runtime's 512 MiB combined-buffer budget, and all backends' prepared inputs/expected answers must fit a 1 GiB host budget. An over-budget backend matrix fails all its otherwise valid cases before allocation. Individually invalid cases retain their own reasons and do not prevent a small valid matrix from running.
- Async staging/results remain limited to 64 MiB and outstanding operations to 1,024. Benchmark CLI limits are 64 entries per list, 256 streams, 64 workers, 10,000 warmups, 100,000 iterations, and one million requested raw samples across backends.
- Output open, write, and flush errors fail the benchmark process.
- Explicit backend selection is preserved. When `both` is requested and Metal is unavailable, the runner reports the omission and continues with CPU results; it never silently substitutes CPU for an explicit Metal request.
- CPU worker count is not GPU thread count. Metal shared-buffer upload/download measurements are host copies, not PCIe or GPU-memory bandwidth measurements.
- The runner makes no speedup, GPU parallelism, or optimized-scheduler claim.
- Ordinary runs are not instrumented; use `--detailed` only when operation lifecycle diagnostics are wanted.

## Validation

Run `bash scripts/validate_milestone5.sh --quick` for a fresh Release build, all CTests, 30 repetitions of async/dependency suites, and ordinary/detailed export checks. On macOS this enables Metal and requests both backends. Running without `--quick` adds the default benchmark matrix and separate CPU ASan/UBSan and TSan builds. Sanitizer runs are correctness checks, not performance measurements; the script preserves failures and never disables leak checking automatically.

`python3 scripts/check_benchmark_results.py OUTPUT_DIR` checks CSV/JSON statistics, sample work counts, timing applicability, operation attribution, per-case record/drop totals, and CPU/GPU field applicability. Use `--allow-failed` only when deliberately checking failed-case exports. Every CTest has a 90-second timeout.

Dependency regressions gate intentionally failing source/consumer streams before accepting event assertions. A deferred-completion stress test checks retirement without relying on unrelated work to wake the retirement thread. Completion readiness is published under the scheduler mutex used by the retirement condition variable; the submission-return handshake still prevents premature retirement.
