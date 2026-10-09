#!/usr/bin/env bash
# Run from the project root or any other directory. Preserves nonzero failures.
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_root"
quick=false
if [[ "${1:-}" == --quick ]]; then quick=true; fi
metal=OFF
backend=cpu
if [[ "$(uname -s)" == Darwin ]]; then metal=ON; backend=both; fi
results="validation_results/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$results"
exec > >(tee "$results/validation.log") 2>&1
set -x
cmake -S . -B build-m5-fixed -DCMAKE_BUILD_TYPE=Release -DGRIDFORGE_ENABLE_METAL="$metal"
cmake --build build-m5-fixed --parallel
ctest --test-dir build-m5-fixed --output-on-failure --timeout 90
ctest --test-dir build-m5-fixed --output-on-failure --timeout 90 --repeat until-fail:30 -R 'gridforge_(dependencies|async)_(cpu|metal)'
./build-m5-fixed/gridforge_bench --backend "$backend" --workload all --mode both --sizes 256,257,4096 --streams 1,2,4 --workers 4 --warmup 1 --iterations 3 --output-dir "$results/smoke"
python3 scripts/check_benchmark_results.py "$results/smoke"
./build-m5-fixed/gridforge_bench --backend "$backend" --workload all --mode both --sizes 256,257 --streams 1,2 --warmup 1 --iterations 2 --detailed --output-dir "$results/detailed"
python3 scripts/check_benchmark_results.py "$results/detailed"
if "$quick"; then exit 0; fi
./build-m5-fixed/gridforge_bench --backend "$backend" --workload all --mode both --output-dir "$results/baseline"
python3 scripts/check_benchmark_results.py "$results/baseline"
cmake -S . -B build-m5-asan -DCMAKE_BUILD_TYPE=Debug -DGRIDFORGE_ENABLE_METAL=OFF -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-m5-asan --parallel
ctest --test-dir build-m5-asan --output-on-failure --timeout 90
cmake -S . -B build-m5-tsan -DCMAKE_BUILD_TYPE=Debug -DGRIDFORGE_ENABLE_METAL=OFF -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread'
cmake --build build-m5-tsan --parallel
ctest --test-dir build-m5-tsan --output-on-failure --timeout 90
