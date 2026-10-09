# Balanced backend ordering

This replacement package applies on top of the fixes for GridForge(3).zip already installed in your checkout.

The uploaded baseline has 2,400 verified samples and its CSV statistics match its summary. However, CPU was measured first for all 600 synchronous pairs, and Metal was measured first for all 600 asynchronous pairs. None of the 60 logical cases changed first backend across its 20 rounds.

The rotating case position canceled the round term in the old backend-index calculation. The new shared schedule helper uses the stable logical case index. For 20 rounds with both backends, every logical case now has CPU first ten times and Metal first ten times. Pairing and total work remain the same.

## Complete replacement files

- `src/bench_main.cpp`: use the corrected schedule and describe it in the summary.
- `src/bench_support.hpp`: add the shared measurement-slot helper.
- `tests/test_bench.cpp`: verify exact schedule pairing, rotation, single-backend behavior, and balanced first-backend counts with odd/even case counts.
- `scripts/check_benchmark_results.py`: verify actual backend chronology in exported samples; reject the earlier fixed-order pattern.
- `docs/benchmark_methodology.md`: document the balancing rule.

## Install and rerun

```sh
cd /Users/anushkasr/Documents/GridForge
unzip -o ~/Downloads/GridForge-M5-Ordering-Fix.zip
bash scripts/validate_milestone5.sh --quick
./build-m5-fixed/gridforge_bench --backend both --workload all --mode both --output-dir bench_results/m5-balanced
python3 scripts/check_benchmark_results.py bench_results/m5-balanced
```

Send `bench_results/m5-balanced/results.csv` and `summary.json` for review. The previously passing runtime and sanitizer checks still apply to their tested code; this change affects benchmark scheduling and its verification.

Validation here: the Release benchmark regression suite passed, the real CPU smoke exported 108 verified samples, the new checker rejected the uploaded baseline for its ordering pattern, and a clearly synthetic balanced-order fixture passed the checker. The schedule regression uses the same helper as the runner. Metal cannot execute in this Linux environment, so a newly measured balanced CPU/Metal comparison still requires the Mac rerun.

The earlier timings are provisional performance observations: CPU had lower matching mean times in all 60 tested cases. This does not establish GPU speedup or identify the cause of the gap. Use the balanced rerun for final performance comparisons.
