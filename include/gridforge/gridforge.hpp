#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace gridforge {

class Stream;
class Event;

namespace detail {
struct RuntimeState;
struct BufferState;
struct StreamState;
struct EventState;
struct DownloadState;
struct TestAccess;
} // namespace detail

inline constexpr std::size_t default_max_allocated_bytes = 512ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t default_worker_count = 4;
inline constexpr std::size_t default_max_outstanding_operations = 1024;
inline constexpr std::size_t default_max_staging_bytes = 64ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t default_operation_metrics_capacity = 4096;

enum class Backend {
    CPU,
    Metal,
};

enum class OperationKind { upload, vector_add, download, event_barrier, event_record, test_task };
enum class OperationStatus { succeeded, failed, skipped };

struct OperationMetric {
    OperationKind kind{OperationKind::test_task};
    Backend backend{Backend::CPU};
    OperationStatus status{OperationStatus::succeeded};
    std::uint64_t stream_id{0};
    std::uint64_t operation_id{0};
    // Host timestamps are nanoseconds from steady_clock's epoch; missing stages are unavailable.
    std::optional<std::int64_t> accepted_steady_ns;
    std::optional<std::int64_t> execution_start_steady_ns;
    std::optional<std::int64_t> host_completion_steady_ns;
    std::optional<std::int64_t> metal_commit_observed_steady_ns;
    std::optional<std::int64_t> metal_completion_observed_steady_ns;
    std::optional<std::int64_t> retirement_publication_steady_ns;
    // Metal command-buffer timestamps and their difference are seconds on Metal's GPU clock.
    std::optional<double> metal_gpu_start_seconds;
    std::optional<double> metal_gpu_end_seconds;
    std::optional<double> metal_gpu_command_buffer_execution_seconds;
};

struct OperationMetricsSnapshot {
    bool enabled{false};
    std::size_t dropped_records{0};
    std::vector<OperationMetric> records;
};

class BackendUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct RuntimeOptions {
    Backend backend{Backend::CPU};
    std::size_t max_allocated_bytes{default_max_allocated_bytes};
    std::size_t worker_count{default_worker_count};
    std::size_t max_outstanding_operations{default_max_outstanding_operations};
    std::size_t max_staging_bytes{default_max_staging_bytes};
    // Disabled by default. Enabled storage is preallocated and bounded per runtime.
    bool enable_operation_metrics{false};
    // Must be in [1, 1048576] when metrics are enabled; excess retired records are dropped and counted.
    std::size_t operation_metrics_capacity{default_operation_metrics_capacity};
};

class Runtime;

class Buffer final {
public:
    Buffer() noexcept;
    ~Buffer();
    Buffer(Buffer&&) noexcept;
    Buffer& operator=(Buffer&&) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    [[nodiscard]] std::size_t size_bytes() const;
    void upload(std::span<const std::byte> source, std::size_t offset = 0);
    void download(std::span<std::byte> destination, std::size_t offset = 0) const;

private:
    struct Impl;
    explicit Buffer(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class Runtime;
    friend class Stream;
};

class Event final {
public:
    Event() noexcept;
    [[nodiscard]] bool is_complete() const;
    void wait() const;
    void rethrow_if_failed() const;

private:
    explicit Event(std::shared_ptr<detail::EventState> state) noexcept;
    std::shared_ptr<detail::EventState> state_;
    friend class Stream;
    friend struct detail::TestAccess;
};

class DownloadResult final {
public:
    DownloadResult() noexcept;
    ~DownloadResult() = default;
    DownloadResult(DownloadResult&&) noexcept = default;
    DownloadResult& operator=(DownloadResult&&) noexcept = default;
    DownloadResult(const DownloadResult&) = delete;
    DownloadResult& operator=(const DownloadResult&) = delete;
    [[nodiscard]] bool is_ready() const;
    void wait() const;
    [[nodiscard]] std::span<const std::byte> get() const;
    [[nodiscard]] std::size_t size_bytes() const;

private:
    explicit DownloadResult(std::shared_ptr<detail::DownloadState> state) noexcept;
    std::shared_ptr<detail::DownloadState> state_;
    friend class Stream;
};

class Stream final {
public:
    Stream() noexcept;
    [[nodiscard]] bool is_closed() const;

    void upload_async(Buffer& destination, std::span<const std::byte> source, std::size_t offset = 0);
    void vector_add_async(const Buffer& a, const Buffer& b, Buffer& c, std::size_t element_count);
    [[nodiscard]] DownloadResult download_async(
        const Buffer& source, std::size_t offset = 0, std::size_t byte_count = std::dynamic_extent);
    [[nodiscard]] Event wait_event(const Event& dependency);
    [[nodiscard]] Event record_event();
    void synchronize() const;

private:
    struct Impl;
    explicit Stream(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
    friend class Runtime;
    friend struct detail::TestAccess;
};

class Runtime final {
public:
    explicit Runtime(RuntimeOptions options = {});
    ~Runtime();
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    [[nodiscard]] Backend backend() const;
    [[nodiscard]] const char* backend_name() const;
    [[nodiscard]] const char* device_name() const;
    [[nodiscard]] std::size_t worker_count() const;
    // Copies then clears retained records; dropped_records is cumulative for the runtime lifetime.
    [[nodiscard]] OperationMetricsSnapshot drain_operation_metrics();
    [[nodiscard]] Buffer create_buffer(std::size_t size_bytes);
    [[nodiscard]] Stream create_stream();

    void vector_add(const Buffer& a, const Buffer& b, Buffer& c, std::size_t element_count);
    void synchronize();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    friend class Buffer;
    friend class Stream;
};

} // namespace gridforge