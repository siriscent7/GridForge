#include "gridforge/gridforge.hpp"
#include "bench_support.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/utsname.h>
#endif

#ifndef GRIDFORGE_GIT_REVISION
#define GRIDFORGE_GIT_REVISION "unavailable"
#endif
#ifndef GRIDFORGE_GIT_DIRTY
#define GRIDFORGE_GIT_DIRTY "unknown"
#endif
#ifndef GRIDFORGE_BUILD_CONFIG
#define GRIDFORGE_BUILD_CONFIG "unknown"
#endif

namespace {

constexpr std::uint32_t default_seed = 0x47524944U;
constexpr std::size_t max_prepared_host_bytes = 1024ULL * 1024ULL * 1024ULL;
constexpr std::size_t max_retained_samples = 1'000'000U;
using Clock = std::chrono::steady_clock;
using Statistics = gridforge::bench_support::Statistics;

struct BenchmarkConfig {
    std::string backend{"cpu"};
    std::string workload{"all"};
    std::string mode{"both"};
    std::vector<std::size_t> sizes{256U, 4'096U, 65'536U, 262'144U, 1'000'000U};
    std::vector<std::size_t> stream_counts{1U, 2U, 4U};
    std::size_t worker_count{4U};
    std::size_t warmup_count{5U};
    std::size_t measured_iterations{20U};
    std::uint32_t seed{default_seed};
    std::string output_dir{"benchmarks"};
    bool detailed{false};
    std::size_t metric_capacity{gridforge::default_operation_metrics_capacity};
};

struct SampleRecord {
    std::string case_id;
    std::size_t iteration_id{0U};
    std::size_t measurement_order{0U};
    std::string backend;
    std::string workload;
    std::string mode;
    std::size_t element_count{0U};
    std::size_t requested_stream_count{1U};
    std::size_t actual_stream_count{0U};
    std::size_t launch_count{0U};
    std::size_t element_additions{0U};
    std::size_t worker_count{0U};
    std::uint32_t seed{0U};
    bool verified{false};
    std::optional<double> end_to_end_ms;
    std::optional<double> submission_ms;
    std::optional<double> resident_compute_ms;
};

struct CasePartition {
    std::size_t offset{0U};
    std::size_t count{0U};
    gridforge::Buffer a;
    gridforge::Buffer b;
    gridforge::Buffer c;
    gridforge::Buffer d;
    gridforge::Stream stream;
    gridforge::Stream intermediate;
    gridforge::Stream consumer;
};

struct BenchmarkCase {
    std::string case_id;
    std::string backend;
    std::string workload;
    std::string mode;
    std::size_t element_count{0U};
    std::size_t requested_stream_count{1U};
    std::size_t actual_stream_count{0U};
    std::size_t launch_count{0U};
    std::size_t element_additions{0U};
    std::size_t worker_count{0U};
    std::uint32_t seed{0U};
    std::size_t warmup_count{0U};
    std::size_t measured_iterations{0U};
    bool failed{false};
    std::string failure_reason;
    double buffer_allocation_ms{0.0};
    std::vector<std::size_t> partition_sizes;
    std::optional<double> stream_creation_ms;
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float> expected;
    std::vector<CasePartition> partitions;
    std::vector<SampleRecord> samples;
    std::size_t metric_records_written{0U};
    std::size_t dropped_metric_records{0U};
};

struct BackendSession {
    std::string backend;
    std::string device;
    double runtime_initialization_ms{0.0};
    std::unique_ptr<gridforge::Runtime> runtime;
    std::vector<BenchmarkCase> cases;
    std::size_t aggregate_buffer_bytes{0U};
    std::size_t cumulative_metric_drops{0U};
};

struct BackendOmission {
    std::string backend;
    std::string reason;
};

std::vector<std::string> split_csv(std::string_view text) {
    std::vector<std::string> values;
    std::string value;
    for (char character : text) {
        if (character == ',') {
            values.push_back(value);
            value.clear();
        } else {
            value.push_back(character);
        }
    }
    values.push_back(value);
    return values;
}

std::string trim(std::string_view text) {
    std::size_t first = 0U;
    while (first < text.size() && (text[first] == ' ' || text[first] == '\t' || text[first] == '\n' || text[first] == '\r')) ++first;
    std::size_t last = text.size();
    while (last > first && (text[last - 1U] == ' ' || text[last - 1U] == '\t' || text[last - 1U] == '\n' || text[last - 1U] == '\r')) --last;
    return std::string(text.substr(first, last - first));
}

std::vector<std::size_t> parse_positive_list(const std::string& text, std::string_view description) {
    if (text.empty()) throw std::invalid_argument(std::string(description) + " list must not be empty.");
    if (text.size() > 4096U) throw std::invalid_argument("Benchmark lists must contain at most 4096 characters.");
    std::vector<std::size_t> values;
    for (const auto& field : split_csv(text)) {
        const std::string token = trim(field);
        if (token.empty()) throw std::invalid_argument(std::string(description) + " list contains an empty field.");
        std::size_t parsed = 0U;
        const auto result = std::from_chars(token.data(), token.data() + token.size(), parsed);
        if (result.ptr != token.data() + token.size() || result.ec != std::errc{} || parsed == 0U) {
            throw std::invalid_argument(std::string(description) + " values must be positive integers without overflow.");
        }
        values.push_back(parsed);
        if (values.size() > 64U) throw std::invalid_argument("Benchmark lists must contain at most 64 entries.");
    }
    return values;
}

std::size_t parse_integer(std::string_view text, std::string_view description) {
    std::size_t parsed = 0U;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ptr != text.data() + text.size() || result.ec != std::errc{} || parsed == 0U) {
        throw std::invalid_argument(std::string(description) + " must be a positive integer.");
    }
    return parsed;
}

BenchmarkConfig parse_arguments(int argc, char** argv) {
    BenchmarkConfig config;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        auto value_after = [&](std::string_view option) -> std::string {
            if (index + 1 >= argc) throw std::invalid_argument("Missing value for " + std::string(option) + ".");
            return argv[++index];
        };
        if (argument == "--backend") config.backend = value_after(argument);
        else if (argument == "--workload") config.workload = value_after(argument);
        else if (argument == "--mode") config.mode = value_after(argument);
        else if (argument == "--sizes") config.sizes = parse_positive_list(value_after(argument), "Benchmark element count");
        else if (argument == "--streams") config.stream_counts = parse_positive_list(value_after(argument), "Benchmark stream count");
        else if (argument == "--workers") config.worker_count = parse_integer(value_after(argument), "--workers");
        else if (argument == "--warmup") config.warmup_count = parse_integer(value_after(argument), "--warmup");
        else if (argument == "--iterations") config.measured_iterations = parse_integer(value_after(argument), "--iterations");
        else if (argument == "--seed") {
            const std::string value = value_after(argument);
            std::uint32_t parsed = 0U;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (result.ptr != value.data() + value.size() || result.ec != std::errc{}) throw std::invalid_argument("--seed must be a nonnegative integer.");
            config.seed = parsed;
        } else if (argument == "--output-dir") config.output_dir = value_after(argument);
        else if (argument == "--detailed") config.detailed = true;
        else if (argument == "--metric-capacity") config.metric_capacity = parse_integer(value_after(argument), "--metric-capacity");
        else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: gridforge_bench [--backend cpu|metal|both] [--workload all|single|streams|dependency|resident]\n"
                         "  [--mode sync|async|both] [--sizes N,N,...] [--streams N,N,...] [--workers N]\n"
                         "  [--warmup N] [--iterations N] [--seed N] [--output-dir PATH] [--detailed] [--metric-capacity N]\n";
            std::exit(0);
        } else throw std::invalid_argument("Unrecognized benchmark argument: " + std::string(argument));
    }

    if (config.backend != "cpu" && config.backend != "metal" && config.backend != "both") throw std::invalid_argument("--backend must be cpu, metal, or both.");
    if (config.mode != "sync" && config.mode != "async" && config.mode != "both") throw std::invalid_argument("--mode must be sync, async, or both.");
    if (config.workload != "all" && config.workload != "single" && config.workload != "streams" && config.workload != "dependency" && config.workload != "resident") {
        throw std::invalid_argument("--workload must be one of: all, single, streams, dependency, resident.");
    }
    if (config.output_dir.empty()) throw std::invalid_argument("Output directory must not be empty.");
    if (config.worker_count > 64U || config.warmup_count > 10'000U || config.measured_iterations > 100'000U ||
        config.metric_capacity > 65'536U || *std::max_element(config.stream_counts.begin(), config.stream_counts.end()) > 256U) {
        throw std::invalid_argument("Benchmark bounds: 64 workers, 256 streams, 10000 warmups, 100000 iterations, 65536 metric records.");
    }
    return config;
}

std::size_t checked_multiply(std::size_t left, std::size_t right, std::string_view description) {
    if (right != 0U && left > std::numeric_limits<std::size_t>::max() / right) throw std::length_error(std::string(description) + " overflows size_t.");
    return left * right;
}

std::size_t checked_add(std::size_t left, std::size_t right, std::string_view description) {
    if (left > std::numeric_limits<std::size_t>::max() - right) throw std::length_error(std::string(description) + " overflows size_t.");
    return left + right;
}

void validate_case_resources(std::size_t count, std::size_t stream_count, std::string_view workload, bool async_mode) {
    if (stream_count == 0U || (workload == "streams" && stream_count > count)) throw std::length_error("Stream count must not exceed the fixed total element count.");
    const std::size_t buffers = workload == "dependency" ? 4U : 3U;
    const std::size_t buffer_bytes = checked_multiply(checked_multiply(count, sizeof(float), "Workload buffer size"), buffers, "Combined workload buffer size");
    if (buffer_bytes > gridforge::default_max_allocated_bytes) throw std::length_error("Requested benchmark size exceeds the default per-runtime 512 MiB buffer allocation limit.");
    if (async_mode && workload != "resident") {
        const std::size_t staged_elements = checked_multiply(count, 3U, "Upload and download staging element count");
        const std::size_t staging_bytes = checked_multiply(staged_elements, sizeof(float), "Async staging and result bytes");
        if (staging_bytes > gridforge::default_max_staging_bytes) throw std::length_error("Requested benchmark size exceeds the default 64 MiB async staging/result limit.");
    }
    const std::size_t outstanding = workload == "streams" ? checked_multiply(stream_count, 4U, "Stream operations")
        : workload == "dependency" ? 9U : workload == "resident" ? 2U : 4U;
    if (async_mode && outstanding > gridforge::default_max_outstanding_operations)
        throw std::length_error("Requested case exceeds the default outstanding-operation limit.");
}

std::vector<float> generate_values(std::size_t count, std::uint32_t seed) {
    std::vector<float> values(count);
    std::mt19937 generator(seed);
    std::uniform_real_distribution<float> distribution(-1000.0F, 1000.0F);
    for (float& value : values) value = distribution(generator);
    return values;
}

std::vector<float> add_vectors(const std::vector<float>& left, const std::vector<float>& right) {
    std::vector<float> result(left.size());
    for (std::size_t index = 0; index < left.size(); ++index) result[index] = left[index] + right[index];
    return result;
}

bool verify_vector(const std::vector<float>& actual, const std::vector<float>& expected) {
    if (actual.size() != expected.size()) return false;
    for (std::size_t index = 0; index < actual.size(); ++index) {
        constexpr float absolute_tolerance = 1.0e-5F;
        constexpr float relative_tolerance = 1.0e-5F;
        if (!std::isfinite(actual[index]) || std::abs(actual[index] - expected[index]) > absolute_tolerance + relative_tolerance * std::abs(expected[index])) return false;
    }
    return true;
}

std::string escape_csv(std::string_view text) {
    if (text.find_first_of(",\"\r\n") == std::string_view::npos) return std::string(text);
    std::string escaped{"\""};
    for (char character : text) {
        if (character == '\"') escaped.push_back('\"');
        escaped.push_back(character);
    }
    escaped.push_back('\"');
    return escaped;
}

std::string escape_json(std::string_view text) {
    std::string escaped;
    for (char character : text) {
        switch (character) {
            case '\\': escaped += "\\\\"; break;
            case '\"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (static_cast<unsigned char>(character) < 0x20U) {
                    std::ostringstream format;
                    format << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(character));
                    escaped += format.str();
                } else escaped.push_back(character);
        }
    }
    return escaped;
}

std::string json_number(double value) {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return output.str();
}

std::string utc_now() {
    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string operating_system() {
#if defined(__APPLE__)
    return "macOS";
#elif defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#else
    return "unknown";
#endif
}

std::string architecture() {
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

std::string compiler_name() {
#if defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

const char* operation_kind_name(gridforge::OperationKind kind) {
    switch (kind) {
        case gridforge::OperationKind::upload: return "upload";
        case gridforge::OperationKind::vector_add: return "vector_add";
        case gridforge::OperationKind::download: return "download";
        case gridforge::OperationKind::event_barrier: return "event_barrier";
        case gridforge::OperationKind::event_record: return "event_record";
        case gridforge::OperationKind::test_task: return "test_task";
    }
    return "unknown";
}

const char* operation_status_name(gridforge::OperationStatus status) {
    switch (status) {
        case gridforge::OperationStatus::succeeded: return "succeeded";
        case gridforge::OperationStatus::failed: return "failed";
        case gridforge::OperationStatus::skipped: return "skipped";
    }
    return "unknown";
}

void write_nullable_integer(std::ofstream& output, const std::optional<std::int64_t>& value) {
    if (value) output << *value;
    else output << "null";
}

void write_nullable_number(std::ofstream& output, const std::optional<double>& value) {
    if (value && std::isfinite(*value)) output << json_number(*value);
    else output << "null";
}

void write_operation_metric(std::ofstream& output, const gridforge::OperationMetric& metric) {
    output << "{\"kind\":\"" << operation_kind_name(metric.kind)
           << "\",\"backend\":\"" << (metric.backend == gridforge::Backend::Metal ? "metal" : "cpu")
           << "\",\"stream_id\":" << metric.stream_id
           << ",\"operation_id\":" << metric.operation_id
           << ",\"terminal_status\":\"" << operation_status_name(metric.status)
           << "\",\"host_timestamps_unit\":\"steady_clock_nanoseconds_since_epoch\",\"accepted\":";
    write_nullable_integer(output, metric.accepted_steady_ns);
    output << ",\"execution_or_submission_start\":";
    write_nullable_integer(output, metric.execution_start_steady_ns);
    output << ",\"host_completion\":";
    write_nullable_integer(output, metric.host_completion_steady_ns);
    output << ",\"metal_commit_observed\":";
    write_nullable_integer(output, metric.metal_commit_observed_steady_ns);
    output << ",\"metal_completion_observed\":";
    write_nullable_integer(output, metric.metal_completion_observed_steady_ns);
    output << ",\"retirement_publication\":";
    write_nullable_integer(output, metric.retirement_publication_steady_ns);
    output << ",\"metal_gpu_start_seconds\":";
    write_nullable_number(output, metric.metal_gpu_start_seconds);
    output << ",\"metal_gpu_end_seconds\":";
    write_nullable_number(output, metric.metal_gpu_end_seconds);
    output << ",\"gpu_command_buffer_execution_duration_seconds\":";
    write_nullable_number(output, metric.metal_gpu_command_buffer_execution_seconds);
    output << '}';
}

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void check_file(std::ofstream& output, const std::string& path);

class OperationWriter {
public:
    OperationWriter(const std::filesystem::path& directory, bool enabled) : enabled_(enabled) {
        if (!enabled_) return;
        output_.open(directory / "operations.jsonl", std::ios::out | std::ios::trunc);
        if (!output_.is_open()) throw std::runtime_error("Failed opening detailed operations.jsonl output.");
    }

    void collect(BackendSession& session, BenchmarkCase& benchmark_case, std::string_view phase,
                 std::size_t iteration, std::size_t measurement_order = 0U) {
        if (!enabled_) return;
        auto snapshot = session.runtime->drain_operation_metrics();
        // The runtime API reports cumulative drops; count only this drain's increment.
        if (snapshot.dropped_records < session.cumulative_metric_drops)
            throw std::logic_error("Runtime metric drop counter moved backwards.");
        const std::size_t drops = snapshot.dropped_records - session.cumulative_metric_drops;
        session.cumulative_metric_drops = snapshot.dropped_records;
        dropped_ = checked_add(dropped_, drops, "Exported metric drop count");
        benchmark_case.dropped_metric_records = checked_add(benchmark_case.dropped_metric_records, drops, "Case metric drop count");
        for (const auto& metric : snapshot.records) {
            output_ << "{\"schema\":\"gridforge.operation.v1\",\"case_id\":\"" << escape_json(benchmark_case.case_id)
                    << "\",\"phase\":\"" << phase << "\",\"iteration_id\":" << iteration
                    << ",\"measurement_order\":" << measurement_order << ",\"operation\":";
            write_operation_metric(output_, metric);
            output_ << "}\n";
        }
        written_ = checked_add(written_, snapshot.records.size(), "Exported metric record count");
        benchmark_case.metric_records_written += snapshot.records.size();
        if (!output_) throw std::runtime_error("Failed writing detailed operations.jsonl output.");
    }

    void finish() { if (enabled_) check_file(output_, "operations.jsonl"); }
    std::size_t written() const noexcept { return written_; }
    std::size_t dropped() const noexcept { return dropped_; }
private:
    bool enabled_;
    std::ofstream output_;
    std::size_t written_{0U};
    std::size_t dropped_{0U};
};

BenchmarkCase make_case(std::size_t ordinal, const std::string& backend, const std::string& workload,
                        const std::string& mode, std::size_t size, std::size_t requested_streams,
                        const BenchmarkConfig& config) {
    BenchmarkCase result;
    std::ostringstream id;
    id << "case-" << std::setw(4) << std::setfill('0') << ordinal;
    result.case_id = id.str();
    result.backend = backend;
    result.workload = workload;
    result.mode = mode;
    result.element_count = size;
    result.requested_stream_count = requested_streams;
    result.worker_count = config.worker_count;
    result.seed = config.seed;
    result.warmup_count = config.warmup_count;
    result.measured_iterations = config.measured_iterations;
    const bool async_mode = mode == "async";
    result.actual_stream_count = async_mode
        ? workload == "streams" ? requested_streams : workload == "dependency" ? 3U : 1U
        : 0U;
    result.launch_count = workload == "streams" ? requested_streams : workload == "dependency" ? 2U : 1U;
    const std::size_t additions_per_element = workload == "dependency" ? 2U : 1U;
    result.element_additions = size <= std::numeric_limits<std::size_t>::max() / additions_per_element
        ? size * additions_per_element : 0U;
    return result;
}

void prepare_case(BenchmarkCase& benchmark_case, gridforge::Runtime& runtime, const BenchmarkConfig& config) {
    const bool async_mode = benchmark_case.mode == "async";
    const std::size_t size = benchmark_case.element_count;
    validate_case_resources(size, benchmark_case.requested_stream_count, benchmark_case.workload, async_mode);

    benchmark_case.left = generate_values(size, config.seed);
    benchmark_case.right = generate_values(size, config.seed ^ 0xA5A5A5A5U);
    if (benchmark_case.workload == "dependency") {
        const auto intermediate = add_vectors(benchmark_case.left, benchmark_case.right);
        benchmark_case.expected = add_vectors(intermediate, benchmark_case.right);
    } else benchmark_case.expected = add_vectors(benchmark_case.left, benchmark_case.right);

    const auto chunk_sizes = benchmark_case.workload == "streams"
        ? gridforge::bench_support::partition_elements(size, benchmark_case.requested_stream_count)
        : std::vector<std::size_t>{size};
    benchmark_case.partition_sizes = chunk_sizes;
    benchmark_case.partitions.reserve(chunk_sizes.size());
    std::size_t offset = 0U;
    const auto allocation_start = Clock::now();
    for (std::size_t count : chunk_sizes) {
        CasePartition partition;
        partition.offset = offset;
        partition.count = count;
        const std::size_t bytes = checked_multiply(count, sizeof(float), "Partition buffer size");
        partition.a = runtime.create_buffer(bytes);
        partition.b = runtime.create_buffer(bytes);
        partition.c = runtime.create_buffer(bytes);
        if (benchmark_case.workload == "dependency") partition.d = runtime.create_buffer(bytes);
        benchmark_case.partitions.push_back(std::move(partition));
        offset += count;
    }
    benchmark_case.buffer_allocation_ms = elapsed_ms(allocation_start, Clock::now());

    const auto stream_start = Clock::now();
    if (async_mode) {
        if (benchmark_case.workload == "dependency") {
            auto& part = benchmark_case.partitions.front();
            part.stream = runtime.create_stream();
            part.intermediate = runtime.create_stream();
            part.consumer = runtime.create_stream();
        } else {
            for (auto& part : benchmark_case.partitions) part.stream = runtime.create_stream();
        }
    }
    if (async_mode) benchmark_case.stream_creation_ms = elapsed_ms(stream_start, Clock::now());

    if (benchmark_case.workload == "resident") {
        auto& part = benchmark_case.partitions.front();
        part.a.upload(std::as_bytes(std::span<const float>(benchmark_case.left)));
        part.b.upload(std::as_bytes(std::span<const float>(benchmark_case.right)));
    }
}

void drain_case(BenchmarkCase& benchmark_case) noexcept {
    if (benchmark_case.mode != "async") return;
    for (auto& part : benchmark_case.partitions) {
        try { part.stream.synchronize(); } catch (...) {}
        try { part.intermediate.synchronize(); } catch (...) {}
        try { part.consumer.synchronize(); } catch (...) {}
    }
}

SampleRecord execute_iteration(BenchmarkCase& benchmark_case, gridforge::Runtime& runtime,
                               std::size_t iteration_id, std::size_t measurement_order = 0U) {
    const bool async_mode = benchmark_case.mode == "async";
    SampleRecord sample;
    sample.case_id = benchmark_case.case_id;
    sample.iteration_id = iteration_id;
    sample.measurement_order = measurement_order;
    sample.backend = benchmark_case.backend;
    sample.workload = benchmark_case.workload;
    sample.mode = benchmark_case.mode;
    sample.element_count = benchmark_case.element_count;
    sample.requested_stream_count = benchmark_case.requested_stream_count;
    sample.actual_stream_count = benchmark_case.actual_stream_count;
    sample.launch_count = benchmark_case.launch_count;
    sample.element_additions = benchmark_case.element_additions;
    sample.worker_count = benchmark_case.worker_count;
    sample.seed = benchmark_case.seed;
    std::vector<float> output(benchmark_case.element_count);

    if (benchmark_case.workload == "resident") {
        auto& part = benchmark_case.partitions.front();
        const auto compute_start = Clock::now();
        if (async_mode) {
            const auto submission_start = Clock::now();
            part.stream.vector_add_async(part.a, part.b, part.c, part.count);
            auto completion = part.stream.record_event();
            sample.submission_ms = elapsed_ms(submission_start, Clock::now());
            completion.wait();
        } else {
            runtime.vector_add(part.a, part.b, part.c, part.count);
            runtime.synchronize();
        }
        sample.resident_compute_ms = elapsed_ms(compute_start, Clock::now());
        part.c.download(std::as_writable_bytes(std::span<float>(output)));
    } else {
        std::vector<gridforge::DownloadResult> results;
        std::vector<std::span<const std::byte>> downloaded;
        if (async_mode) {
            results.reserve(benchmark_case.partitions.size());
            downloaded.reserve(benchmark_case.partitions.size());
        }
        const auto end_to_end_start = Clock::now();
        if (async_mode) {
            const auto submission_start = Clock::now();
            for (auto& part : benchmark_case.partitions) {
                const auto left = std::span<const float>(benchmark_case.left).subspan(part.offset, part.count);
                const auto right = std::span<const float>(benchmark_case.right).subspan(part.offset, part.count);
                if (benchmark_case.workload == "dependency") {
                    part.stream.upload_async(part.a, std::as_bytes(left));
                    part.stream.upload_async(part.b, std::as_bytes(right));
                    part.stream.vector_add_async(part.a, part.b, part.c, part.count);
                    const auto produced = part.stream.record_event();
                    (void)part.intermediate.wait_event(produced);
                    part.intermediate.vector_add_async(part.c, part.b, part.d, part.count);
                    const auto transformed = part.intermediate.record_event();
                    (void)part.consumer.wait_event(transformed);
                    results.push_back(part.consumer.download_async(part.d));
                } else {
                    part.stream.upload_async(part.a, std::as_bytes(left));
                    part.stream.upload_async(part.b, std::as_bytes(right));
                    part.stream.vector_add_async(part.a, part.b, part.c, part.count);
                    results.push_back(part.stream.download_async(part.c));
                }
            }
            sample.submission_ms = elapsed_ms(submission_start, Clock::now());

            for (const auto& result : results) downloaded.push_back(result.get());
            sample.end_to_end_ms = elapsed_ms(end_to_end_start, Clock::now());
            for (std::size_t index = 0; index < downloaded.size(); ++index) {
                const auto& part = benchmark_case.partitions[index];
                std::memcpy(output.data() + part.offset, downloaded[index].data(), downloaded[index].size());
            }
        } else {
            for (auto& part : benchmark_case.partitions) {
                const auto left = std::span<const float>(benchmark_case.left).subspan(part.offset, part.count);
                const auto right = std::span<const float>(benchmark_case.right).subspan(part.offset, part.count);
                part.a.upload(std::as_bytes(left));
                part.b.upload(std::as_bytes(right));
                if (benchmark_case.workload == "dependency") {
                    runtime.vector_add(part.a, part.b, part.c, part.count);
                    runtime.vector_add(part.c, part.b, part.d, part.count);
                    part.d.download(std::as_writable_bytes(std::span<float>(output).subspan(part.offset, part.count)));
                } else {
                    runtime.vector_add(part.a, part.b, part.c, part.count);
                    part.c.download(std::as_writable_bytes(std::span<float>(output).subspan(part.offset, part.count)));
                }
            }
            sample.end_to_end_ms = elapsed_ms(end_to_end_start, Clock::now());
        }
    }

    if (!verify_vector(output, benchmark_case.expected)) throw std::runtime_error("Benchmark output verification failed.");
    sample.verified = true;
    return sample;
}

std::vector<std::string> modes_for(const BenchmarkConfig& config) {
    return config.mode == "both" ? std::vector<std::string>{"sync", "async"} : std::vector<std::string>{config.mode};
}

void append_case(std::vector<BenchmarkCase>& cases, std::size_t& next_id, const std::string& backend,
                 const std::string& workload, std::size_t size, std::size_t stream_count,
                 const BenchmarkConfig& config) {
    for (const auto& mode : modes_for(config)) cases.push_back(make_case(next_id++, backend, workload, mode, size, stream_count, config));
}

void mark_failure(BenchmarkCase& benchmark_case, const std::exception& error) {
    benchmark_case.failed = true;
    benchmark_case.failure_reason = error.what();
    std::cerr << "[benchmark] " << benchmark_case.case_id << " failed: " << error.what() << '\n';
}

void release_case_storage(BenchmarkCase& benchmark_case) noexcept {
    std::vector<float>().swap(benchmark_case.left);
    std::vector<float>().swap(benchmark_case.right);
    std::vector<float>().swap(benchmark_case.expected);
    std::vector<CasePartition>().swap(benchmark_case.partitions);
}

void append_backend_cases(const BenchmarkConfig& config, const std::string& backend,
                          std::vector<BenchmarkCase>& cases, std::size_t& next_id) {
    for (std::size_t size : config.sizes) {
        if (config.workload == "all" || config.workload == "single") append_case(cases, next_id, backend, "single", size, 1U, config);
        if (config.workload == "all" || config.workload == "streams") {
            for (std::size_t streams : config.stream_counts) append_case(cases, next_id, backend, "streams", size, streams, config);
        }
        if (config.workload == "all" || config.workload == "dependency") append_case(cases, next_id, backend, "dependency", size, 1U, config);
        if (config.workload == "all" || config.workload == "resident") append_case(cases, next_id, backend, "resident", size, 1U, config);
    }
}

// All cases retain their buffers, inputs and expected answers for rotating measured rounds.
// Check the whole matrix before generating any inputs or allocating any case buffers.
void preflight_cases(std::vector<BackendSession>& sessions, const BenchmarkConfig& config) {
    std::size_t prepared_host_bytes = 0U;
    std::size_t requested_samples = 0U;
    for (auto& session : sessions) {
        std::size_t host_bytes = 0U;
        for (auto& benchmark_case : session.cases) {
            requested_samples = checked_add(requested_samples, config.measured_iterations, "Requested samples");
            if (requested_samples > max_retained_samples)
                throw std::length_error("Benchmark matrix exceeds the one-million retained-sample limit.");
            try {
                validate_case_resources(benchmark_case.element_count, benchmark_case.requested_stream_count,
                                        benchmark_case.workload, benchmark_case.mode == "async");
                const auto bytes = checked_multiply(benchmark_case.element_count, sizeof(float), "Workload buffer size");
                session.aggregate_buffer_bytes = checked_add(session.aggregate_buffer_bytes,
                    checked_multiply(bytes, benchmark_case.workload == "dependency" ? 4U : 3U, "Case buffers"), "Aggregate case buffers");
                host_bytes = checked_add(host_bytes, checked_multiply(bytes, 3U, "Prepared case inputs"), "Prepared backend inputs");
            } catch (const std::exception& error) { mark_failure(benchmark_case, error); }
        }
        if (session.aggregate_buffer_bytes > gridforge::default_max_allocated_bytes) {
            const std::length_error error("Aggregate prepared-case buffers exceed the per-runtime 512 MiB buffer allocation limit; reduce the case matrix.");
            for (auto& benchmark_case : session.cases) if (!benchmark_case.failed) mark_failure(benchmark_case, error);
        } else {
            prepared_host_bytes = checked_add(prepared_host_bytes, host_bytes, "Prepared host inputs across backends");
        }
    }
    if (prepared_host_bytes > max_prepared_host_bytes) {
        const std::length_error error("Prepared inputs and expected answers exceed the combined 1 GiB host-memory limit.");
        for (auto& session : sessions)
            for (auto& benchmark_case : session.cases) if (!benchmark_case.failed) mark_failure(benchmark_case, error);
    }
}

void prepare_backend_cases(BackendSession& session, const BenchmarkConfig& config, OperationWriter& metrics) {
    auto& cases = session.cases;
    auto& runtime = *session.runtime;

    for (auto& benchmark_case : cases) {
        if (benchmark_case.failed) continue;
        try { prepare_case(benchmark_case, runtime, config); }
        catch (const std::exception& error) {
            drain_case(benchmark_case);
            release_case_storage(benchmark_case);
            mark_failure(benchmark_case, error);
        }
        metrics.collect(session, benchmark_case, "preparation", 0U);
    }
    for (auto& benchmark_case : cases) {
        if (benchmark_case.failed) continue;
        for (std::size_t warmup = 0; warmup < benchmark_case.warmup_count; ++warmup) {
            try { (void)execute_iteration(benchmark_case, runtime, 0U); }
            catch (const std::exception& error) {
                drain_case(benchmark_case);
                mark_failure(benchmark_case, error);
                metrics.collect(session, benchmark_case, "warmup", warmup + 1U);
                break;
            }
            drain_case(benchmark_case);
            metrics.collect(session, benchmark_case, "warmup", warmup + 1U);
        }
    }
}

std::vector<double> values_for(const BenchmarkCase& benchmark_case, const std::optional<double> SampleRecord::*member) {
    std::vector<double> values;
    for (const auto& sample : benchmark_case.samples) if (sample.verified && sample.*member) values.push_back(*(sample.*member));
    return values;
}

void write_statistics(std::ofstream& output, const Statistics& stats) {
    output << "{\"count\":" << stats.sample_count
           << ",\"min\":" << (stats.sample_count ? json_number(stats.min) : "null")
           << ",\"median\":" << (stats.sample_count ? json_number(stats.median) : "null")
           << ",\"mean\":" << (stats.sample_count ? json_number(stats.mean) : "null")
           << ",\"sample_stddev\":" << (stats.sample_count ? json_number(stats.sample_stddev) : "null")
           << ",\"max\":" << (stats.sample_count ? json_number(stats.max) : "null")
           << ",\"p95_nearest_rank\":" << (stats.sample_count ? json_number(stats.p95) : "null") << '}';
}

void write_metric_statistics(std::ofstream& output, const BenchmarkCase& benchmark_case,
                             const std::optional<double> SampleRecord::*member) {
    const auto values = values_for(benchmark_case, member);
    if (values.empty()) output << "null";
    else write_statistics(output, gridforge::bench_support::compute_statistics(values));
}

void check_file(std::ofstream& output, const std::string& path) {
    output.flush();
    if (!output) throw std::runtime_error("Failed writing benchmark output file: " + path);
}

void write_csv(const std::filesystem::path& path, const std::vector<BackendSession>& sessions) {
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) throw std::runtime_error("Failed opening benchmark output file: " + path.string());
    output << "case_id,iteration_id,measurement_order,backend,workload,mode,element_count,requested_stream_count,actual_stream_count,launch_count,element_additions,worker_count,seed,verified,end_to_end_ms,submission_ms,resident_compute_ms\n";
    for (const auto& session : sessions) {
        for (const auto& benchmark_case : session.cases) {
            for (const auto& sample : benchmark_case.samples) {
                if (!sample.verified) continue;
                output << escape_csv(sample.case_id) << ',' << sample.iteration_id << ',' << sample.measurement_order << ',' << sample.backend << ','
                       << sample.workload << ',' << sample.mode << ',' << sample.element_count << ','
                       << sample.requested_stream_count << ',' << sample.actual_stream_count << ','
                       << sample.launch_count << ',' << sample.element_additions << ',' << sample.worker_count << ','
                       << sample.seed << ",true,";
                if (sample.end_to_end_ms) output << json_number(*sample.end_to_end_ms);
                output << ',';
                if (sample.submission_ms) output << json_number(*sample.submission_ms);
                output << ',';
                if (sample.resident_compute_ms) output << json_number(*sample.resident_compute_ms);
                output << '\n';
            }
        }
    }
    check_file(output, path.string());
}

void write_summary_json(const std::filesystem::path& path, const BenchmarkConfig& config,
               const std::vector<BackendSession>& sessions,
               const std::vector<BackendOmission>& omissions,
               const OperationWriter& metrics,
               const std::string& run_utc) {
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) throw std::runtime_error("Failed opening benchmark output file: " + path.string());
    const std::string git_dirty = GRIDFORGE_GIT_DIRTY;
    output << "{\n  \"schema\":\"gridforge.benchmark.v2\",\n"
        << "  \"run_utc\":\"" << run_utc << "\",\n"
           << "  \"backend\":\"" << escape_json(config.backend) << "\",\n"
           << "  \"workload\":\"" << escape_json(config.workload) << "\",\n"
           << "  \"mode\":\"" << escape_json(config.mode) << "\",\n"
           << "  \"worker_count\":" << config.worker_count << ",\n"
           << "  \"warmup_count\":" << config.warmup_count << ",\n"
           << "  \"measured_iterations\":" << config.measured_iterations << ",\n"
        << "  \"seed\":" << config.seed << ",\n"
        << "  \"instrumented\":" << (config.detailed ? "true" : "false") << ",\n"
        << "  \"host\":{\"os\":\"" << operating_system() << "\",\"architecture\":\"" << architecture() << "\"";
#if !defined(_WIN32)
    struct utsname host_info{};
    if (uname(&host_info) == 0) output << ",\"os_release\":\"" << escape_json(host_info.release) << "\"";
#endif
    output << ",\"compiler\":\"" << escape_json(compiler_name())
        << "\",\"build_type\":\"" << escape_json(GRIDFORGE_BUILD_CONFIG) << "\"},\n"
        << "  \"git\":{\"revision\":\"" << escape_json(GRIDFORGE_GIT_REVISION) << "\",\"dirty\":";
    if (git_dirty == "true" || git_dirty == "false") output << git_dirty;
    else output << "null";
    output << "},\n  \"runtime_limits\":{\"max_allocated_bytes\":" << gridforge::default_max_allocated_bytes
        << ",\"max_staging_bytes\":" << gridforge::default_max_staging_bytes
        << ",\"max_outstanding_operations\":" << gridforge::default_max_outstanding_operations
        << ",\"operation_metrics_capacity\":" << config.metric_capacity
        << ",\"max_prepared_host_bytes\":" << max_prepared_host_bytes
        << ",\"max_retained_samples\":" << max_retained_samples << "},\n  \"sizes\":[";
    for (std::size_t index = 0; index < config.sizes.size(); ++index) { if (index) output << ','; output << config.sizes[index]; }
    output << "],\n  \"stream_counts\":[";
    for (std::size_t index = 0; index < config.stream_counts.size(); ++index) { if (index) output << ','; output << config.stream_counts[index]; }
    output << "],\n  \"timing\":{\"unit\":\"ms\",\"clock\":\"std::chrono::steady_clock\","
              "\"end_to_end_boundary\":\"first upload/submission through all required downloads available\","
              "\"submission_boundary\":\"first asynchronous API submission through last submission return\","
              "\"resident_compute_boundary\":\"launch through final completion; input upload/output download excluded\","
              "\"verification_boundary\":\"outside execution timing\"},\n"
           << "  \"measured_case_order\":\"case index rotates by round; first backend alternates per logical case and round; each case is drained\",\n"
           << "  \"backends\":[";
    for (std::size_t index = 0; index < sessions.size(); ++index) {
        if (index) output << ',';
        output << "{\"backend\":\"" << escape_json(sessions[index].backend)
               << "\",\"device\":\"" << escape_json(sessions[index].device)
               << "\",\"aggregate_buffer_bytes\":" << sessions[index].aggregate_buffer_bytes
               << ",\"runtime_initialization_ms\":" << json_number(sessions[index].runtime_initialization_ms) << '}';
    }
    output << "],\n  \"backend_omissions\":[";
    for (std::size_t index = 0; index < omissions.size(); ++index) {
        if (index) output << ',';
        output << "{\"backend\":\"" << escape_json(omissions[index].backend)
               << "\",\"reason\":\"" << escape_json(omissions[index].reason) << "\"}";
    }
    output << "],\n  \"cases\":[\n";
    bool first_case = true;
    for (const auto& session : sessions) {
        for (const auto& benchmark_case : session.cases) {
            if (!first_case) output << ",\n";
            first_case = false;
            output << "    {\"case_id\":\"" << escape_json(benchmark_case.case_id)
                   << "\",\"backend\":\"" << escape_json(benchmark_case.backend)
                   << "\",\"workload\":\"" << escape_json(benchmark_case.workload)
                   << "\",\"mode\":\"" << escape_json(benchmark_case.mode)
                   << "\",\"element_count\":" << benchmark_case.element_count
                   << ",\"partitions\":[";
            for (std::size_t part = 0; part < benchmark_case.partition_sizes.size(); ++part) {
                if (part) output << ',';
                output << benchmark_case.partition_sizes[part];
            }
            output << "],\"requested_stream_count\":" << benchmark_case.requested_stream_count
                   << ",\"actual_stream_count\":" << benchmark_case.actual_stream_count
                   << ",\"launch_count\":" << benchmark_case.launch_count
                   << ",\"element_additions\":" << benchmark_case.element_additions
                   << ",\"worker_count\":" << benchmark_case.worker_count
                   << ",\"seed\":" << benchmark_case.seed
                   << ",\"warmup_count\":" << benchmark_case.warmup_count
                   << ",\"measured_iterations\":" << benchmark_case.measured_iterations
                   << ",\"buffer_allocation_ms\":" << json_number(benchmark_case.buffer_allocation_ms)
                     << ",\"stream_creation_ms\":";
                 write_nullable_number(output, benchmark_case.stream_creation_ms);
                 output << ",\"status\":\"" << (benchmark_case.failed ? "failed" : "verified")
                   << "\",\"failure_reason\":\"" << escape_json(benchmark_case.failure_reason)
                   << "\",\"verified_sample_count\":" << benchmark_case.samples.size()
                   << ",\"metric_records_written\":" << benchmark_case.metric_records_written
                   << ",\"dropped_metric_records\":" << benchmark_case.dropped_metric_records
                   << ",\"statistics\":{\"end_to_end_ms\":";
            write_metric_statistics(output, benchmark_case, &SampleRecord::end_to_end_ms);
            output << ",\"submission_ms\":";
            write_metric_statistics(output, benchmark_case, &SampleRecord::submission_ms);
            output << ",\"resident_compute_ms\":";
            write_metric_statistics(output, benchmark_case, &SampleRecord::resident_compute_ms);
            output << "}}";
        }
    }
    output << "\n  ],\n  \"instrumentation\":{\"enabled\":" << (config.detailed ? "true" : "false")
           << ",\"capacity_per_runtime\":" << (config.detailed ? config.metric_capacity : 0U)
           << ",\"retained_records_written\":" << metrics.written()
           << ",\"dropped_records\":" << metrics.dropped() << ",\"complete\":";
    if (config.detailed) output << (metrics.dropped() == 0U ? "true" : "false");
    else output << "null";
    output << ",\"records_file\":" << (config.detailed ? "\"operations.jsonl\"" : "null")
           << ",\"record_schema\":" << (config.detailed ? "\"gridforge.operation.v1\"" : "null")
           << ",\"coverage\":\"accepted asynchronous operations only; synchronous calls and rejected submissions are excluded\""
           << ",\"steady_clock_unit\":\"nanoseconds since steady_clock epoch\",\"gpu_time_unit\":\"seconds\","
              "\"gpu_duration_label\":\"GPU command-buffer execution duration\","
              "\"timestamp_boundaries\":{\"accepted\":\"successful scheduler acceptance\","
              "\"execution_or_submission_start\":\"worker begins execution or async submission call\","
              "\"host_completion\":\"host operation closure completes, or non-Metal deferred completion is observed\","
              "\"metal_commit_observed\":\"async submission call returns after command-buffer commit\","
              "\"metal_completion_observed\":\"runtime callback observes terminal Metal completion\","
              "\"retirement_publication\":\"reservations/accounting retire before event/result readiness publication\"}}}\n";
    check_file(output, path.string());
}

} // namespace

int main(int argc, char** argv) {
    try {
        const BenchmarkConfig config = parse_arguments(argc, argv);
        const std::string run_utc = utc_now();
        const std::filesystem::path output_dir(config.output_dir);
        std::filesystem::create_directories(output_dir);
        OperationWriter metrics(output_dir, config.detailed);
        const std::vector<std::string> backends = config.backend == "both"
            ? std::vector<std::string>{"cpu", "metal"} : std::vector<std::string>{config.backend};
        std::vector<BackendSession> sessions;
        std::vector<BackendOmission> omissions;
        std::size_t next_id = 1U;
        for (const auto& backend : backends) {
            try {
                const auto setup_start = Clock::now();
                const auto selected = backend == "metal" ? gridforge::Backend::Metal : gridforge::Backend::CPU;
                gridforge::RuntimeOptions options;
                options.backend = selected;
                options.worker_count = config.worker_count;
                options.enable_operation_metrics = config.detailed;
                options.operation_metrics_capacity = config.metric_capacity;
                BackendSession session;
                session.backend = backend;
                session.runtime = std::make_unique<gridforge::Runtime>(options);
                session.runtime_initialization_ms = elapsed_ms(setup_start, Clock::now());
                session.device = session.runtime->device_name();
                append_backend_cases(config, backend, session.cases, next_id);
                sessions.push_back(std::move(session));
            }
            catch (const gridforge::BackendUnavailable& error) {
                if (config.backend == "both") {
                    std::cout << "Metal benchmark backend unavailable; continuing only with CPU results: " << error.what() << '\n';
                    omissions.push_back({backend, error.what()});
                    continue;
                }
                throw;
            }
        }

        preflight_cases(sessions, config);
        for (auto& session : sessions) prepare_backend_cases(session, config, metrics);
        const std::size_t matching_case_count = sessions.empty() ? 0U : sessions.front().cases.size();
        for (const auto& session : sessions) {
            if (session.cases.size() != matching_case_count) throw std::logic_error("Backend benchmark case matrices are not aligned.");
        }
        std::size_t next_measurement_order = 1U;
        for (std::size_t round = 0; round < config.measured_iterations && matching_case_count != 0U; ++round) {
            for (std::size_t offset = 0; offset < matching_case_count; ++offset) {
                for (std::size_t backend_offset = 0; backend_offset < sessions.size(); ++backend_offset) {
                    const auto slot = gridforge::bench_support::measurement_slot(
                        round, offset, matching_case_count, backend_offset, sessions.size());
                    auto& session = sessions[slot.backend_index];
                    auto& benchmark_case = session.cases[slot.case_index];
                    if (!benchmark_case.failed) {
                        const std::size_t measurement_order = next_measurement_order++;
                        try {
                            benchmark_case.samples.push_back(execute_iteration(benchmark_case, *session.runtime,
                                round + 1U, measurement_order));
                        } catch (const std::exception& error) {
                            drain_case(benchmark_case);
                            mark_failure(benchmark_case, error);
                        }
                    } else {
                        ++next_measurement_order;
                    }
                    drain_case(benchmark_case);
                    metrics.collect(session, benchmark_case, "measured", round + 1U, next_measurement_order - 1U);
                }
            }
        }

        metrics.finish();
        for (auto& session : sessions) {
            for (auto& benchmark_case : session.cases) release_case_storage(benchmark_case);
            if (config.detailed) {
                std::cout << "Operation metrics (" << session.backend << "): dropped=" << session.cumulative_metric_drops << '\n';
            }
        }
        if (config.detailed && metrics.dropped() != 0U)
            std::cout << "Detailed operation metrics are incomplete: capacity overflow dropped records.\n";

        std::size_t total_cases = 0U;
        for (const auto& session : sessions) total_cases += session.cases.size();
        write_csv(output_dir / "results.csv", sessions);
        write_summary_json(output_dir / "summary.json", config, sessions, omissions, metrics, run_utc);

        std::size_t sample_count = 0U;
        std::size_t failure_count = 0U;
        for (const auto& session : sessions) {
            for (const auto& benchmark_case : session.cases) {
                sample_count += benchmark_case.samples.size();
                if (benchmark_case.failed) {
                    ++failure_count;
                    std::cerr << "[failure] case=" << benchmark_case.case_id << " backend=" << benchmark_case.backend
                              << " workload=" << benchmark_case.workload << " mode=" << benchmark_case.mode
                              << " size=" << benchmark_case.element_count << ": " << benchmark_case.failure_reason << '\n';
                }
            }
        }
        std::cout << "Benchmark suite completed. Results written to " << output_dir << '\n'
                  << "Cases: " << total_cases << '\n'
                  << "Verified measured samples: " << sample_count << '\n';
        if (failure_count != 0U) {
            std::cerr << "Benchmark failure summary: " << failure_count << " of " << total_cases
                      << " cases failed. Details are in summary.json.\n";
        }
        return total_cases == 0U || failure_count != 0U ? 2 : 0;
    } catch (const std::exception& error) {
        std::cerr << "Benchmark error: " << error.what() << '\n';
        return 1;
    }
}
