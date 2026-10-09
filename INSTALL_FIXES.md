# Install the fixes for GridForge(3).zip

These files are based on your latest uploaded Copilot source, `GridForge(3).zip`.

## Use the replacement files in your existing checkout

Download `GridForge-M5-Replacements.zip`. From your existing GridForge directory, extract it there so the files replace the matching paths and the new scripts are added:

```sh
cd /Users/anushkasr/Documents/GridForge
unzip -o ~/Downloads/GridForge-M5-Replacements.zip
bash scripts/validate_milestone5.sh --quick
```

The archive contains complete files, not snippets. If you have changed those files since uploading GridForge(3).zip, compare those changes before replacing them. Alternatively, extract `GridForge-M5-Fixed-Full.zip` separately and validate its `GridForge-Copilot-Fixed` directory.

## Changed and new files

| Path | Change |
| --- | --- |
| `src/runtime.cpp` | Publish completion readiness under the scheduler mutex used by the retirement condition variable, preventing a lost wake-up. Preserve the submission-return/completion handshake. |
| `src/bench_main.cpp` | Validate all retained case buffers before allocating case inputs, bound prepared host memory and raw samples, periodically drain metrics, stream attributed operations, and count cumulative drop increments correctly. |
| `tests/test_dependencies.cpp` | Gate intentionally failing streams before accepting their event assertions; add 1,000 deferred completions without unrelated work that could wake a stranded completion. |
| `tests/test_bench.cpp` | Check aggregate rejection, per-case/phase/iteration attribution, exact drop counts, long-run draining, and output failures; isolate temporary directories. |
| `CMakeLists.txt` | Pass the built benchmark's actual path to its test, build it before the test target, and give the benchmark test the same 90-second timeout. |
| `scripts/check_benchmark_results.py` | New independent standard-library CSV/JSON/JSONL checker. |
| `scripts/validate_milestone5.sh` | New Mac/Linux Release validation script with repeated tests and benchmark export checks; optional full matrix and CPU sanitizer builds. |
| `README.md`, `docs/benchmark_methodology.md` | Document the corrected resource checks and detailed export format. |
| `.gitignore` | Exclude generated validation results. |
| `INSTALL_FIXES.md` | These installation and validation notes. |

## Export format change

`summary.json` uses schema `gridforge.benchmark.v2`. Detailed operation records previously embedded in `instrumentation.records` are now streamed to the file named by `instrumentation.records_file`, `operations.jsonl`. Each line has schema `gridforge.operation.v1`, `case_id`, `phase`, `iteration_id`, `measurement_order`, and the original runtime metric under `operation`.

Metrics remain asynchronous-only, as in the Copilot source. They are drained after each preparation, warmup, and measured iteration outside execution timing. Capacity defaults to 4,096 records; `--metric-capacity N` permits explicit overflow probes. The runtime API reports cumulative drops, so the runner adds only each drain's increment. Long runs can write more than 4,096 total records without retaining them all in memory.

## Validation in this environment

- Linux x86_64, GCC 13.3.0; fresh CPU-only Release build: all four available tests passed, with the three Metal tests skipped.
- Fresh CPU ASan/UBSan and TSan builds: all four available tests passed; Metal tests skipped.
- Dependency suite passed 100 consecutive Release runs, each including 1,000 deferred completions (100,000 total).
- The validation script's quick path passed end to end, including 30 repeats of the CPU async and dependency suites.
- Ordinary smoke: 36 cases, 108 verified samples. Detailed smoke: 20 cases, 40 verified samples and 162 attributed operations with no drops.
- Default CPU matrix: 60 cases, 1,200 verified samples.
- Capacity-two dependency probe: two verified measured samples plus one warmup; six retained operations, 21 dropped records, and explicit `complete=false`.
- The aggregate-over-budget probe rejected all 12 cases before input/buffer preparation, returned exit 2, and exported zero measured samples.
- The Python checker passed the ordinary, detailed, overflow, default, and deliberate failed-matrix exports. The full-source archive includes logs and exported evidence.

ASan/UBSan used `ASAN_OPTIONS=detect_leaks=0` because this environment's LeakSanitizer previously reported unsupported process inspection. Leak checking remains unverified here, and the delivered script does not disable it automatically. Metal cannot be compiled or executed in this Linux environment; validate this updated runtime on your Mac. Existing Metal results in the original upload predate these fixes.

Run the full validation when ready:

```sh
bash scripts/validate_milestone5.sh
```

Milestone 5 should remain open until the updated Mac/Metal tests and intended benchmark review are complete.
