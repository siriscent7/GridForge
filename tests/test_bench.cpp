#include "bench_support.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace {

bool file_exists(const std::filesystem::path& path) {
    return std::filesystem::exists(path) && std::filesystem::is_regular_file(path);
}

std::string shell_quote(const std::string& value) {
    std::string quoted{"\""};
    for (char ch : value) {
        if (ch == '\"' || ch == '\\' || ch == '$' || ch == '`') quoted += '\\';
        quoted += ch;
    }
    return quoted + "\"";
}

std::string make_command(const std::string& executable, const std::string& args) {
    return shell_quote(executable) + " " + args;
}

bool run_and_check(const std::string& command, int expected_code) {
    const int code = std::system(command.c_str());
#if defined(_WIN32)
    return code == expected_code;
#else
    return WIFEXITED(code) && WEXITSTATUS(code) == expected_code;
#endif
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

} // namespace

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    // Test the exact schedule used by the runner, including case rotation.
    for (std::size_t case_count : {5U, 6U, 60U}) {
        std::vector<std::size_t> cpu_first(case_count, 0U);
        for (std::size_t round = 0; round < 20; ++round) {
            std::vector<bool> visited(case_count, false);
            for (std::size_t offset = 0; offset < case_count; ++offset) {
                const auto first = gridforge::bench_support::measurement_slot(round, offset, case_count, 0U, 2U);
                const auto second = gridforge::bench_support::measurement_slot(round, offset, case_count, 1U, 2U);
                if (visited[first.case_index] || first.case_index != second.case_index ||
                    first.backend_index == second.backend_index) {
                    std::cerr << "Measurement schedule omitted or repeated a paired case.\n"; return 1;
                }
                visited[first.case_index] = true;
                cpu_first[first.case_index] += first.backend_index == 0U;
                const auto single = gridforge::bench_support::measurement_slot(round, offset, case_count, 0U, 1U);
                if (single.case_index != first.case_index || single.backend_index != 0U) {
                    std::cerr << "Single-backend schedule differs from paired logical ordering.\n"; return 1;
                }
            }
        }
        if (std::any_of(cpu_first.begin(), cpu_first.end(), [](std::size_t count) { return count != 10U; })) {
            std::cerr << "CPU and Metal do not each run first in half the rounds for every logical case.\n"; return 1;
        }
    }
    const auto known = gridforge::bench_support::compute_statistics({1.0, 2.0, 3.0, 4.0, 5.0});
    if (known.sample_count != 5U || known.min != 1.0 || known.median != 3.0 || known.mean != 3.0 ||
        std::abs(known.sample_stddev - std::sqrt(2.5)) > 1.0e-12 || known.max != 5.0 || known.p95 != 5.0) {
        std::cerr << "Known sample statistics are incorrect.\n";
        return 1;
    }
    const auto uneven = gridforge::bench_support::partition_elements(257U, 3U);
    if (uneven != std::vector<std::size_t>{86U, 86U, 85U}) {
        std::cerr << "Uneven stream partitioning is incorrect.\n";
        return 1;
    }

    const fs::path executable = argc > 1 ? fs::path(argv[1]) : fs::current_path() / "gridforge_bench";
    const fs::path temp_dir = fs::temp_directory_path() / ("gridforge_bench_tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temp_dir);

    const fs::path csv_file = temp_dir / "results.csv";
    const fs::path summary_file = temp_dir / "summary.json";
    const std::string cmd = make_command(executable.string(), "--backend cpu --workload single --sizes 256,257,4096 --warmup 1 --iterations 1 --output-dir " + shell_quote(temp_dir.string()));
    if (!run_and_check(cmd, 0)) {
        std::cerr << "CPU benchmark smoke run failed.\n";
        return 1;
    }
    if (!file_exists(csv_file) || !file_exists(summary_file)) {
        std::cerr << "Benchmark output files were not created.\n";
        return 1;
    }

    const std::string valid_streams = make_command(executable.string(), "--backend cpu --workload streams --sizes 256 --streams 1,2 --warmup 1 --iterations 1 --output-dir " + shell_quote(temp_dir.string()));
    if (!run_and_check(valid_streams, 0)) {
        std::cerr << "Valid stream-count list was rejected.\n";
        return 1;
    }

    const std::vector<std::string> invalid_lists{
        "--sizes ''", "--sizes ',256'", "--sizes '256,'", "--sizes '256,,257'",
        "--streams ''", "--streams ',1'", "--streams '1,'", "--streams '1,,2'",
        "--sizes '256,nope'", "--sizes '256,18446744073709551616'", "--sizes '0'", "--streams '0'"};
    for (const std::string& invalid_list : invalid_lists) {
        const std::string invalid = make_command(executable.string(), "--backend cpu --workload single " + invalid_list +
            " --warmup 1 --iterations 1 --output-dir " + shell_quote(temp_dir.string()));
        if (!run_and_check(invalid, 1)) {
            std::cerr << "Invalid benchmark list was accepted: " << invalid_list << '\n';
            return 1;
        }
    }

    const std::string header = read_all(csv_file);
    if (header.find("case_id,iteration_id,measurement_order,backend,workload,mode") != 0U ||
        header.find(",sync,") == std::string::npos || header.find(",async,") == std::string::npos) {
        std::cerr << "CSV output schema is invalid.\n";
        return 1;
    }

    if (read_all(summary_file).find("\"schema\":\"gridforge.benchmark.v2\"") == std::string::npos) {
        std::cerr << "JSON output schema is invalid.\n";
        return 1;
    }
    if (read_all(summary_file).find("\"instrumentation\":{\"enabled\":false") == std::string::npos) {
        std::cerr << "Ordinary benchmark run unexpectedly enabled instrumentation.\n";
        return 1;
    }

    const fs::path detailed_dir = temp_dir / "detailed";
    const std::string detailed = make_command(executable.string(), "--backend cpu --workload single --mode async --sizes 16,17 "
        "--warmup 1 --iterations 1 --detailed --output-dir " + shell_quote(detailed_dir.string()));
    if (!run_and_check(detailed, 0)) {
        std::cerr << "Detailed metrics smoke run failed.\n";
        return 1;
    }
    const std::string detailed_json = read_all(detailed_dir / "summary.json");
    const std::string operations = read_all(detailed_dir / "operations.jsonl");
    if (detailed_json.find("\"instrumentation\":{\"enabled\":true") == std::string::npos ||
        detailed_json.find("\"dropped_records\":0") == std::string::npos ||
        operations.find("\"kind\":\"upload\"") == std::string::npos ||
        operations.find("\"terminal_status\":\"succeeded\"") == std::string::npos ||
        operations.find("\"metal_gpu_start_seconds\":null") == std::string::npos) {
        std::cerr << "Detailed operation metric export is incomplete or fabricated a CPU GPU timestamp.\n";
        return 1;
    }

    if (operations.find("\"case_id\":\"case-0001\"") == std::string::npos ||
        operations.find("\"case_id\":\"case-0002\"") == std::string::npos ||
        operations.find("\"phase\":\"warmup\",\"iteration_id\":1,\"measurement_order\":0") == std::string::npos ||
        operations.find("\"phase\":\"measured\",\"iteration_id\":1,\"measurement_order\":2") == std::string::npos ||
        std::count(operations.begin(), operations.end(), '\n') != 16 ||
        detailed_json.find("\"retained_records_written\":16") == std::string::npos ||
        detailed_json.find("\"records_file\":\"operations.jsonl\"") == std::string::npos) {
        std::cerr << "Detailed operation attribution or streamed record count is incorrect.\n"; return 1;
    }

    const fs::path dropped_dir = temp_dir / "dropped-metrics";
    const std::string dropped = make_command(executable.string(), "--backend cpu --workload dependency --mode async --sizes 1 "
        "--warmup 1 --iterations 1 --detailed --metric-capacity 2 --output-dir " + shell_quote(dropped_dir.string()));
    if (!run_and_check(dropped, 0)) {
        std::cerr << "Metrics overflow benchmark run failed unexpectedly.\n";
        return 1;
    }
    const std::string dropped_json = read_all(dropped_dir / "summary.json");
    if (dropped_json.find("\"capacity_per_runtime\":2") == std::string::npos ||
        dropped_json.find("\"complete\":false") == std::string::npos ||
        dropped_json.find("\"dropped_records\":14") == std::string::npos) {
        std::cerr << "Bounded metrics overflow was not reported as incomplete.\n";
        return 1;
    }

    const fs::path long_dir = temp_dir / "long-detailed";
    const std::string long_run = make_command(executable.string(), "--backend cpu --workload dependency --mode async --sizes 1 "
        "--warmup 1 --iterations 460 --detailed --output-dir " + shell_quote(long_dir.string()));
    if (!run_and_check(long_run, 0) || read_all(long_dir / "summary.json").find("\"retained_records_written\":4149") == std::string::npos ||
        read_all(long_dir / "summary.json").find("\"complete\":true") == std::string::npos) {
        std::cerr << "Periodic draining dropped records across otherwise bounded iterations.\n"; return 1;
    }
    const fs::path aggregate_dir = temp_dir / "aggregate-budget";
    const std::string aggregate = make_command(executable.string(), "--backend cpu --workload all --mode both --sizes 4000000 "
        "--streams 1,2,4 --warmup 1 --iterations 1 --output-dir " + shell_quote(aggregate_dir.string()));
    if (!run_and_check(aggregate, 2)) { std::cerr << "Aggregate buffer matrix was accepted.\n"; return 1; }
    const std::string aggregate_csv = read_all(aggregate_dir / "results.csv");
    const std::string aggregate_json = read_all(aggregate_dir / "summary.json");
    if (std::count(aggregate_csv.begin(), aggregate_csv.end(), '\n') != 1 ||
        aggregate_json.find("Aggregate prepared-case buffers") == std::string::npos ||
        aggregate_json.find("\"buffer_allocation_ms\":0") == std::string::npos ||
        aggregate_json.find("\"status\":\"verified\"") != std::string::npos) {
        std::cerr << "Aggregate matrix was not rejected before all case allocation.\n"; return 1;
    }

    const std::string max_size = std::to_string(std::numeric_limits<std::size_t>::max());
    const fs::path mixed_dir = temp_dir / "mixed";
    const std::string mixed = make_command(executable.string(), "--backend cpu --workload single --sizes 256," + max_size +
        " --warmup 1 --iterations 1 --output-dir " + shell_quote(mixed_dir.string()));
    if (!run_and_check(mixed, 2)) {
        std::cerr << "Mixed successful/oversized cases did not return the failure status.\n";
        return 1;
    }
    const std::string mixed_csv = read_all(mixed_dir / "results.csv");
    const std::string mixed_json = read_all(mixed_dir / "summary.json");
    if (mixed_csv.find("case_id,iteration_id") != 0U ||
        std::count(mixed_csv.begin(), mixed_csv.end(), '\n') != 3 ||
        mixed_json.find("\"status\":\"failed\"") == std::string::npos ||
        mixed_json.find("Workload buffer size overflows size_t.") == std::string::npos) {
        std::cerr << "Mixed-case results did not preserve successful and failed case details.\n";
        return 1;
    }

    const fs::path over_limit_dir = temp_dir / "over_limit";
    const std::string over_limit = make_command(executable.string(), "--backend cpu --workload single --sizes 50000000"
        " --warmup 1 --iterations 1 --output-dir " + shell_quote(over_limit_dir.string()));
    if (!run_and_check(over_limit, 2) ||
        read_all(over_limit_dir / "summary.json").find("512 MiB buffer allocation limit") == std::string::npos) {
        std::cerr << "Over-limit benchmark input was not rejected before allocation.\n";
        return 1;
    }

    const std::string oversized = make_command(executable.string(), "--backend cpu --workload single --sizes " + max_size +
        " --warmup 1 --iterations 1 --output-dir " + shell_quote(temp_dir.string()));
    if (!run_and_check(oversized, 2)) {
        std::cerr << "Oversized benchmark input was not rejected before allocation.\n";
        return 1;
    }

    const fs::path matrix_dir = temp_dir / "matrix";
    const std::string matrix = make_command(executable.string(), "--backend cpu --workload all --mode both "
        "--sizes 256,257,4096 --streams 1,2,4 --warmup 1 --iterations 3 --output-dir " + shell_quote(matrix_dir.string()));
    if (!run_and_check(matrix, 0)) {
        std::cerr << "Raw-sample matrix benchmark failed.\n";
        return 1;
    }
    const std::string matrix_csv = read_all(matrix_dir / "results.csv");
    const std::size_t raw_rows = static_cast<std::size_t>(std::count(matrix_csv.begin(), matrix_csv.end(), '\n')) - 1U;
    if (raw_rows != 108U || std::count(matrix_csv.begin(), matrix_csv.end(), '\n') != 109) {
        std::cerr << "Expected 108 verified raw samples across 36 cases.\n";
        return 1;
    }
    const std::string matrix_json = read_all(matrix_dir / "summary.json");
    if (matrix_json.find("\"verified_sample_count\":3") == std::string::npos ||
        matrix_json.find("\"p95_nearest_rank\"") == std::string::npos ||
        matrix_json.find("\"element_additions\":257") == std::string::npos) {
        std::cerr << "Per-case sample statistics or workload metrics are missing.\n";
        return 1;
    }

    const fs::path blocked_dir = temp_dir / "blocked-output";
    fs::create_directories(blocked_dir / "results.csv");
    const std::string blocked = make_command(executable.string(), "--backend cpu --workload single --sizes 256 "
        "--mode sync --warmup 1 --iterations 1 --output-dir " + shell_quote(blocked_dir.string()));
    if (!run_and_check(blocked, 1)) {
        std::cerr << "Benchmark output-open failure did not return nonzero.\n";
        return 1;
    }
    const fs::path blocked_summary_dir = temp_dir / "blocked-summary-output";
    fs::create_directories(blocked_summary_dir / "summary.json");
    const std::string blocked_summary = make_command(executable.string(), "--backend cpu --workload single --sizes 256 "
        "--mode sync --warmup 1 --iterations 1 --output-dir " + shell_quote(blocked_summary_dir.string()));
    if (!run_and_check(blocked_summary, 1)) {
        std::cerr << "Benchmark summary-open failure did not return nonzero.\n";
        return 1;
    }

    const fs::path blocked_operations_dir = temp_dir / "blocked-operations";
    fs::create_directories(blocked_operations_dir / "operations.jsonl");
    const std::string blocked_operations = make_command(executable.string(), "--backend cpu --workload single --sizes 16 "
        "--mode async --warmup 1 --iterations 1 --detailed --output-dir " + shell_quote(blocked_operations_dir.string()));
    if (!run_and_check(blocked_operations, 1)) { std::cerr << "Operation-output open failure did not fail.\n"; return 1; }
#if defined(__linux__)
    const fs::path full_dir = temp_dir / "full-operations";
    fs::create_directories(full_dir);
    fs::create_symlink("/dev/full", full_dir / "operations.jsonl");
    const std::string full_operations = make_command(executable.string(), "--backend cpu --workload single --sizes 16 "
        "--mode async --warmup 1 --iterations 1 --detailed --output-dir " + shell_quote(full_dir.string()));
    if (!run_and_check(full_operations, 1)) { std::cerr << "Operation-output flush failure did not fail.\n"; return 1; }
#endif
    fs::remove_all(temp_dir);
    std::cout << "PASS: benchmark timing, aggregate memory, attributed operations, overflow, and output failures\n";
    return 0;
}
