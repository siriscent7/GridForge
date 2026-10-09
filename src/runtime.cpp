#include "gridforge/gridforge.hpp"
#include "test_hooks.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <unordered_map>
#include <vector>

#if defined(GRIDFORGE_HAS_METAL)
#include "metal_bridge.hpp"
#endif

namespace gridforge::detail {

using CompletionCallback = std::function<void(std::exception_ptr, std::optional<double>, std::optional<double>)>;

std::int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Operation;
struct StreamState;
struct RuntimeState;

enum class AccessMode { read, write };

struct AccessReservation {
    std::uint64_t owner_id;
    AccessMode mode;
    std::size_t count;
    std::size_t stream_position;
};

struct BufferState {
    BufferState(std::shared_ptr<RuntimeState> runtime, std::size_t size);
    ~BufferState();

    std::weak_ptr<RuntimeState> runtime;
    Backend backend;
    std::size_t bytes;
    bool accounted{false};
    std::mutex synchronous_mutex;
    std::vector<std::byte> cpu_storage;
#if defined(GRIDFORGE_HAS_METAL)
    std::unique_ptr<MetalBuffer> metal_storage;
    std::shared_ptr<MetalDevice> metal_device;
#endif
    std::vector<AccessReservation> reservations;
};

struct EventState {
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    bool done{false};
    std::exception_ptr error;
    std::weak_ptr<RuntimeState> runtime;
    std::uint64_t operation_id{0};
    std::uint64_t stream_id{0};
    std::size_t stream_position{0};
    bool dependency_eligible{false};
    std::unordered_map<std::uint64_t, std::size_t> coverage;
    std::vector<std::weak_ptr<Operation>> subscribers;
};

struct StagingLease {
    StagingLease(std::shared_ptr<RuntimeState> runtime, std::size_t amount)
        : runtime(std::move(runtime)), bytes(amount) {}
    ~StagingLease();

    std::weak_ptr<RuntimeState> runtime;
    std::size_t bytes;
    bool active{false};
};

struct DownloadState {
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    bool done{false};
    std::exception_ptr error;
    std::vector<std::byte> bytes;
    std::shared_ptr<StagingLease> lease;
};

struct Operation {
    std::function<void()> execute;
    std::function<std::shared_ptr<void>(CompletionCallback)> submit_async;
    OperationKind kind{OperationKind::test_task};
    std::shared_ptr<EventState> event;
    std::shared_ptr<DownloadState> download;
    std::vector<std::pair<std::shared_ptr<BufferState>, AccessMode>> accesses;
    std::shared_ptr<StagingLease> staging;
    std::size_t stream_position{0};
    std::uint64_t ordering_id{0};
    bool skipped{false};
    std::optional<std::int64_t> accepted_ns;
    std::optional<std::int64_t> execution_start_ns;
    std::optional<std::int64_t> host_completion_ns;
    std::optional<std::int64_t> metal_commit_observed_ns;
    std::optional<std::int64_t> metal_completion_observed_ns;
    std::optional<double> metal_gpu_start_seconds;
    std::optional<double> metal_gpu_end_seconds;
    std::shared_ptr<EventState> dependency;
    std::unordered_map<std::uint64_t, std::size_t> coverage;
    std::unordered_map<std::uint64_t, std::size_t> dependency_coverage;
    std::mutex completion_mutex;
    std::exception_ptr completion_error;
    std::atomic<bool> completion_ready{false};
    std::atomic<bool> completion_signaled{false};
    bool completion_received{false};
    bool submission_returned{false};
    std::shared_ptr<void> backend_submission;
};

struct InFlightOperation {
    std::shared_ptr<StreamState> stream;
    std::shared_ptr<Operation> operation;
};

struct StreamState {
    std::weak_ptr<RuntimeState> runtime;
    std::uint64_t id{0};
    std::deque<std::shared_ptr<Operation>> queue;
    bool running{false};
    bool blocked{false};
    bool closed{false};
    std::exception_ptr failure;
    std::size_t failed_at{0};
    std::size_t last_accepted{0};
    std::size_t retired{0};
    std::unordered_map<std::uint64_t, std::size_t> coverage;
    std::vector<std::weak_ptr<Operation>> active_barriers;
};

struct RuntimeState : std::enable_shared_from_this<RuntimeState> {
    RuntimeState(Backend backend, std::size_t workers, std::size_t max_operations,
                 std::size_t max_staging, std::size_t max_allocations,
                 bool enable_metrics, std::size_t metrics_capacity)
        : backend(backend), worker_limit(workers), operation_limit(max_operations),
          staging_limit(max_staging), allocation_limit(max_allocations),
          metrics_enabled(enable_metrics), metrics_capacity(metrics_capacity) {
        if (max_operations == 0) {
            throw std::invalid_argument("Maximum outstanding operations must be greater than zero.");
        }
        if (enable_metrics) {
            if (metrics_capacity == 0 || metrics_capacity > 1'048'576U) {
                throw std::invalid_argument("Operation metrics capacity must be between 1 and 1048576 records.");
            }
            metrics_records.reserve(metrics_capacity);
        }
        if (workers != 0) {
            threads.reserve(workers);
            try {
                for (std::size_t i = 0; i < workers; ++i) {
                    threads.emplace_back([this] { worker_loop(); });
                }
                retirement_thread = std::thread([this] { retirement_loop(); });
            } catch (...) {
                {
                    std::lock_guard lock(mutex);
                    stopping = true;
                }
                ready_cv.notify_all();
                gpu_cv.notify_all();
                for (auto& thread : threads) if (thread.joinable()) thread.join();
                if (retirement_thread.joinable()) retirement_thread.join();
                throw;
            }
        }
    }

    ~RuntimeState() { shutdown(); }

    void start_shutdown() noexcept {
        std::unique_lock lock(mutex);
        accepting = false;
        idle_cv.wait(lock, [this] { return outstanding == 0; });
        stopping = true;
        ready_cv.notify_all();
        gpu_cv.notify_all();
        lock.unlock();
        join_threads();
        if (retirement_thread.joinable()) retirement_thread.join();
        lock.lock();
        for (auto& stream : streams) stream->closed = true;
    }

    void shutdown() noexcept {
        if (shutdown_started.exchange(true)) return;
        start_shutdown();
    }

    void join_threads() noexcept {
        for (auto& thread : threads) {
            if (thread.joinable()) {
                if (thread.get_id() == std::this_thread::get_id()) {
                    // Runtime destruction from one of its worker callbacks is unsupported;
                    // detaching here avoids self-join termination while state remains alive.
                    thread.detach();
                } else {
                    thread.join();
                }
            }
        }
        threads.clear();
    }

    void worker_loop() noexcept {
        for (;;) {
            std::shared_ptr<StreamState> stream;
            std::shared_ptr<Operation> operation;
            {
                std::unique_lock lock(mutex);
                ready_cv.wait(lock, [this] { return stopping || !ready.empty(); });
                if (ready.empty()) {
                    if (stopping) return;
                    continue;
                }
                stream = std::move(ready.front());
                ready.pop_front();
                if (stream->queue.empty()) continue;
                operation = stream->queue.front();
                if (operation->dependency && !operation->dependency->done) {
                    stream->blocked = true;
                    continue;
                }
                if (operation->dependency && operation->dependency->error) {
                    auto failure = operation->dependency->error;
                    operation->skipped = true;
                    stream->running = true;
                    stream->queue.pop_front();
                    lock.unlock();
                    std::lock_guard retirement_lock(mutex);
                    finish_active_locked(stream, operation, std::move(failure));
                    continue;
                }
                stream->running = true;
                stream->queue.pop_front();
            }

            if (operation->submit_async) {
                bool inserted = false;
                try {
                    if (metrics_enabled) operation->execution_start_ns = steady_now_ns();
                    {
                        std::lock_guard lock(mutex);
                        in_flight.push_back({stream, operation});
                        inserted = true;
                    }
                    std::weak_ptr<RuntimeState> weak_runtime = weak_from_this();
                    std::weak_ptr<Operation> weak_operation = operation;
                    auto backend_submission = operation->submit_async([weak_runtime, weak_operation](
                        std::exception_ptr error, std::optional<double> gpu_start, std::optional<double> gpu_end) noexcept {
                        try {
                            auto submitted = weak_operation.lock();
                            if (!submitted || submitted->completion_signaled.exchange(true)) return;
                            bool notify = false;
                            {
                                std::lock_guard lock(submitted->completion_mutex);
                                submitted->completion_error = std::move(error);
                                submitted->completion_received = true;
                                if (auto runtime = weak_runtime.lock(); runtime && runtime->metrics_enabled) {
                                    if (runtime->backend == Backend::Metal) {
                                        submitted->metal_completion_observed_ns = steady_now_ns();
                                        submitted->metal_gpu_start_seconds = gpu_start;
                                        submitted->metal_gpu_end_seconds = gpu_end;
                                    } else {
                                        submitted->host_completion_ns = steady_now_ns();
                                    }
                                }
                                notify = submitted->submission_returned;
                            }
                            if (notify) {
                                if (auto runtime = weak_runtime.lock()) runtime->publish_completion_ready(submitted);
                            }
                        } catch (...) {
                            // Completion forwarding is allocation-free and cannot fail due to operation limits.
                        }
                    });
                    bool notify = false;
                    {
                        std::lock_guard lock(operation->completion_mutex);
                        operation->backend_submission = std::move(backend_submission);
                        if (metrics_enabled && backend == Backend::Metal) operation->metal_commit_observed_ns = steady_now_ns();
                        operation->submission_returned = true;
                        notify = operation->completion_received;
                    }
                    if (notify) publish_completion_ready(operation);
                } catch (...) {
                    const auto error = std::current_exception();
                    if (metrics_enabled && !operation->host_completion_ns) operation->host_completion_ns = steady_now_ns();
                    if (inserted) {
                        std::lock_guard lock(mutex);
                        auto it = std::find_if(in_flight.begin(), in_flight.end(), [&](const InFlightOperation& entry) {
                            return entry.operation == operation;
                        });
                        if (it != in_flight.end()) {
                            in_flight.erase(it);
                            finish_active_locked(stream, operation, error);
                        }
                    } else {
                        std::lock_guard lock(mutex);
                        finish_active_locked(stream, operation, error);
                    }
                }
                continue;
            }

            std::exception_ptr failure;
            if (metrics_enabled) operation->execution_start_ns = steady_now_ns();
            try {
                operation->execute();
            } catch (...) {
                failure = std::current_exception();
            }
            if (metrics_enabled) operation->host_completion_ns = steady_now_ns();

            {
                std::lock_guard lock(mutex);
                finish_active_locked(stream, operation, failure);
            }
        }
    }

    void finish_active_locked(const std::shared_ptr<StreamState>& stream,
                              const std::shared_ptr<Operation>& operation,
                              std::exception_ptr failure) noexcept {
        retire_operation(*stream, operation, failure);
        stream->running = false;
        if (failure) stream->failure = failure;
        if (failure && stream->failed_at == 0) stream->failed_at = operation->stream_position;
        if (stream->failure) {
            while (!stream->queue.empty()) {
                auto skipped = std::move(stream->queue.front());
                stream->queue.pop_front();
                skipped->skipped = true;
                retire_operation(*stream, skipped, stream->failure);
            }
        } else if (!stream->queue.empty()) {
            try {
                ready.push_back(stream);
                ready_cv.notify_one();
            } catch (...) {
                stream->failure = std::current_exception();
                stream->failed_at = stream->queue.front()->stream_position;
                while (!stream->queue.empty()) {
                    auto skipped = std::move(stream->queue.front());
                    stream->queue.pop_front();
                    skipped->skipped = true;
                    retire_operation(*stream, skipped, stream->failure);
                }
            }
        }
        idle_cv.notify_all();
    }

    void publish_completion_ready(const std::shared_ptr<Operation>& operation) {
        // Publish under the same mutex used by gpu_cv's predicate/wait transition.
        // An atomic flag alone cannot prevent a notification between that check and sleep.
        // The completion mutex must be released first: retirement takes these locks separately.
        {
            std::lock_guard lock(mutex);
            operation->completion_ready.store(true, std::memory_order_release);
        }
        gpu_cv.notify_one();
    }

    void retirement_loop() noexcept {
        for (;;) {
            InFlightOperation completed;
            {
                std::unique_lock lock(mutex);
                gpu_cv.wait(lock, [this] {
                    return (stopping && in_flight.empty()) ||
                        std::any_of(in_flight.begin(), in_flight.end(), [](const InFlightOperation& entry) {
                            return entry.operation->completion_ready.load(std::memory_order_acquire);
                        });
                });
                if (stopping && in_flight.empty()) return;
                auto it = std::find_if(in_flight.begin(), in_flight.end(), [](const InFlightOperation& entry) {
                    return entry.operation->completion_ready.load(std::memory_order_acquire);
                });
                if (it == in_flight.end()) continue;
                completed = std::move(*it);
                in_flight.erase(it);
            }
            std::exception_ptr failure;
            {
                std::lock_guard lock(completed.operation->completion_mutex);
                failure = completed.operation->completion_error;
            }
            {
                std::lock_guard lock(mutex);
                finish_active_locked(completed.stream, completed.operation, std::move(failure));
            }
        }
    }

    void release_reservations(const Operation& operation, std::uint64_t owner_id) noexcept {
        for (const auto& [buffer, mode] : operation.accesses) {
            auto& entries = buffer->reservations;
            auto found = std::find_if(entries.begin(), entries.end(), [owner_id, mode, &operation](const AccessReservation& reservation) {
                return reservation.owner_id == owner_id && reservation.mode == mode &&
                    reservation.stream_position == operation.stream_position;
            });
            if (found != entries.end()) {
                if (--found->count == 0) entries.erase(found);
            }
        }
    }

    void retire_operation(StreamState& stream, const std::shared_ptr<Operation>& operation,
                          std::exception_ptr error) noexcept {
        release_reservations(*operation, stream.id);
        operation->execute = {};
        operation->submit_async = {};
        operation->backend_submission.reset();
        operation->staging.reset();
        ++stream.retired;
        --outstanding;
        if (metrics_enabled) {
            OperationMetric metric;
            metric.kind = operation->kind;
            metric.backend = backend;
            metric.status = operation->skipped ? OperationStatus::skipped
                : error ? OperationStatus::failed : OperationStatus::succeeded;
            metric.stream_id = stream.id;
            metric.operation_id = operation->ordering_id;
            metric.accepted_steady_ns = operation->accepted_ns;
            metric.execution_start_steady_ns = operation->execution_start_ns;
            metric.host_completion_steady_ns = operation->host_completion_ns;
            metric.metal_commit_observed_steady_ns = operation->metal_commit_observed_ns;
            metric.metal_completion_observed_steady_ns = operation->metal_completion_observed_ns;
            metric.retirement_publication_steady_ns = steady_now_ns();
            metric.metal_gpu_start_seconds = operation->metal_gpu_start_seconds;
            metric.metal_gpu_end_seconds = operation->metal_gpu_end_seconds;
            if (operation->metal_gpu_start_seconds && operation->metal_gpu_end_seconds &&
                std::isfinite(*operation->metal_gpu_start_seconds) && std::isfinite(*operation->metal_gpu_end_seconds) &&
                *operation->metal_gpu_end_seconds >= *operation->metal_gpu_start_seconds) {
                metric.metal_gpu_command_buffer_execution_seconds =
                    *operation->metal_gpu_end_seconds - *operation->metal_gpu_start_seconds;
            }
            std::lock_guard metrics_lock(metrics_mutex);
            if (metrics_records.size() < metrics_capacity) metrics_records.push_back(std::move(metric));
            else if (dropped_metric_records != std::numeric_limits<std::size_t>::max()) ++dropped_metric_records;
        }
        if (operation->event) {
            {
                std::lock_guard event_lock(operation->event->mutex);
                operation->event->error = error;
                operation->event->done = true;
            }
            for (auto& weak_subscriber : operation->event->subscribers) {
                auto barrier = weak_subscriber.lock();
                if (!barrier) continue;
                for (auto& subscriber_stream : streams) {
                    if (subscriber_stream->blocked && !subscriber_stream->queue.empty() &&
                        subscriber_stream->queue.front() == barrier) {
                        subscriber_stream->blocked = false;
                        try {
                            ready.push_back(subscriber_stream);
                            ready_cv.notify_one();
                        } catch (...) {
                            // The barrier remains queued; shutdown/runtime synchronization still observes it.
                            subscriber_stream->failure = std::current_exception();
                            subscriber_stream->failed_at = barrier->stream_position;
                        }
                    }
                }
            }
            operation->event->subscribers.clear();
            operation->event->cv.notify_all();
        }
        if (operation->download) {
            std::weak_ptr<DownloadState> completed_download = operation->download;
            {
                std::lock_guard result_lock(operation->download->mutex);
                operation->download->error = error;
                operation->download->done = true;
                if (error) operation->download->lease.reset();
            }
            operation->download.reset();
            if (auto result = completed_download.lock()) result->cv.notify_all();
        }
        if (operation->dependency) {
            auto& barriers = stream.active_barriers;
            barriers.erase(std::remove_if(barriers.begin(), barriers.end(), [&](const auto& weak) {
                auto barrier = weak.lock();
                return !barrier || barrier == operation;
            }), barriers.end());
        }
    }

    void reserve_accesses(const std::vector<std::pair<std::shared_ptr<BufferState>, AccessMode>>& accesses,
                          std::uint64_t owner_id, std::size_t stream_position,
                          const std::unordered_map<std::uint64_t, std::size_t>* coverage = nullptr) {
        std::size_t committed = 0;
        try {
            for (const auto& [buffer, mode] : accesses) {
                for (const auto& existing : buffer->reservations) {
                    if (existing.owner_id != owner_id &&
                        (existing.mode == AccessMode::write || mode == AccessMode::write)) {
                        bool ordered = false;
                        if (coverage != nullptr) {
                            const auto covered = coverage->find(existing.owner_id);
                            ordered = covered != coverage->end() && covered->second >= existing.stream_position;
                        }
                        if (!ordered) {
                            throw std::logic_error("Buffer access conflicts with pending work not ordered before this operation.");
                        }
                    }
                }
                buffer->reservations.push_back({owner_id, mode, 1, stream_position});
                ++committed;
            }
        } catch (...) {
            for (std::size_t i = 0; i < committed; ++i) {
                const auto& [buffer, mode] = accesses[i];
                auto found = std::find_if(buffer->reservations.begin(), buffer->reservations.end(), [owner_id, mode, stream_position](const AccessReservation& reservation) {
                    return reservation.owner_id == owner_id && reservation.mode == mode &&
                        reservation.stream_position == stream_position;
                });
                if (found != buffer->reservations.end() && --found->count == 0) buffer->reservations.erase(found);
            }
            throw;
        }
    }

    mutable std::mutex mutex;
    std::condition_variable ready_cv;
    std::condition_variable gpu_cv;
    std::condition_variable idle_cv;
    bool accepting{true};
    bool stopping{false};
    std::atomic<bool> shutdown_started{false};
    Backend backend;
    std::size_t worker_limit;
    std::size_t operation_limit;
    std::size_t staging_limit;
    std::size_t allocation_limit;
    bool metrics_enabled{false};
    std::size_t metrics_capacity{0};
    mutable std::mutex metrics_mutex;
    std::vector<OperationMetric> metrics_records;
    std::size_t dropped_metric_records{0};
    std::atomic<std::size_t> allocated_bytes{0};
    std::size_t outstanding{0};
    std::atomic<std::size_t> staging_bytes{0};
    std::size_t next_stream_id{1};
    std::uint64_t next_sync_id{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t next_ordering_id{1};
    std::deque<std::shared_ptr<StreamState>> ready;
    std::vector<std::shared_ptr<StreamState>> streams;
    std::vector<std::thread> threads;
    std::thread retirement_thread;
    std::vector<InFlightOperation> in_flight;
#if defined(GRIDFORGE_HAS_METAL)
    std::shared_ptr<MetalDevice> metal;
#endif
};

StagingLease::~StagingLease() {
    if (active) {
        if (auto owner = runtime.lock()) owner->staging_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
}

BufferState::BufferState(std::shared_ptr<RuntimeState> owner, std::size_t size)
    : runtime(owner), backend(owner->backend), bytes(size) {
#if defined(GRIDFORGE_HAS_METAL)
    metal_device = owner->metal;
#endif
    if (backend == Backend::CPU) cpu_storage.resize(size);
}

BufferState::~BufferState() {
    if (accounted) {
        if (auto owner = runtime.lock()) owner->allocated_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
}

} // namespace gridforge::detail

namespace gridforge {

struct Runtime::Impl {
    explicit Impl(RuntimeOptions requested) : options(requested) {
        switch (options.backend) {
        case Backend::CPU:
#if defined(__aarch64__) || defined(__arm64__)
            device = "Host CPU (arm64)";
#elif defined(__x86_64__)
            device = "Host CPU (x86_64)";
#else
            device = "Host CPU";
#endif
            break;
        case Backend::Metal:
#if defined(GRIDFORGE_HAS_METAL)
            {
                auto metal_device = detail::create_metal_device();
                device = metal_device->name();
                scheduler = std::make_shared<detail::RuntimeState>(options.backend, options.worker_count,
                    options.max_outstanding_operations, options.max_staging_bytes, options.max_allocated_bytes,
                    options.enable_operation_metrics, options.operation_metrics_capacity);
                scheduler->metal = std::shared_ptr<detail::MetalDevice>(std::move(metal_device));
            }
            return;
#else
            throw BackendUnavailable("Metal backend is unavailable: GridForge was built without Metal support.");
#endif
            break;
        default:
            throw std::invalid_argument("Unknown GridForge backend selection.");
        }
        scheduler = std::make_shared<detail::RuntimeState>(options.backend, options.worker_count,
            options.max_outstanding_operations, options.max_staging_bytes, options.max_allocated_bytes,
            options.enable_operation_metrics, options.operation_metrics_capacity);
    }

    RuntimeOptions options;
    std::string device;
    std::shared_ptr<detail::RuntimeState> scheduler;
};

struct Buffer::Impl {
    explicit Impl(std::shared_ptr<detail::BufferState> state) : state(std::move(state)) {}
    std::shared_ptr<detail::BufferState> state;
};

struct Stream::Impl {
    explicit Impl(std::shared_ptr<detail::StreamState> state) : state(std::move(state)) {}
    std::shared_ptr<detail::StreamState> state;
};

namespace {

std::shared_ptr<detail::RuntimeState> get_runtime(const std::shared_ptr<detail::StreamState>& stream) {
    if (!stream) throw std::logic_error("Operation on an empty GridForge stream.");
    auto runtime = stream->runtime.lock();
    if (!runtime) throw std::logic_error("GridForge stream is closed because its runtime was destroyed.");
    return runtime;
}

void complete_immediate(const std::shared_ptr<detail::EventState>& event, std::exception_ptr error = {}) {
    if (!event) return;
    {
        std::lock_guard lock(event->mutex);
        event->error = error;
        event->done = true;
    }
    event->cv.notify_all();
}

void accept_operation(const std::shared_ptr<detail::RuntimeState>& runtime,
                      const std::shared_ptr<detail::StreamState>& stream,
                      const std::shared_ptr<detail::Operation>& operation) {
    std::lock_guard lock(runtime->mutex);
    if (!runtime->accepting || stream->closed) throw std::logic_error("Cannot submit work to a closed GridForge runtime or stream.");
    if (stream->failure) throw std::logic_error("Cannot submit work to a failed GridForge stream.");
    if (runtime->outstanding >= runtime->operation_limit) {
        throw std::length_error("Maximum outstanding operation limit reached; submission was rejected.");
    }
    bool reservations_committed = false;
    bool ready_committed = false;
    bool queue_committed = false;
    bool subscriber_committed = false;
    bool barrier_committed = false;
    const auto position = stream->last_accepted + 1;
    if (runtime->next_ordering_id == 0) throw std::overflow_error("Runtime operation ordering identifier space exhausted.");
    const auto ordering_id = runtime->next_ordering_id;
    std::unordered_map<std::uint64_t, std::size_t> next_coverage;
    try {
        next_coverage = stream->coverage;
        if (operation->dependency) {
            operation->dependency_coverage = operation->dependency->coverage;
            for (const auto& [source, prefix] : operation->dependency->coverage) {
                auto& current = next_coverage[source];
                current = std::max(current, prefix);
            }
        }
        operation->coverage = next_coverage;
        if (operation->event) {
            next_coverage[stream->id] = position;
            operation->event->runtime = runtime;
            operation->event->operation_id = ordering_id;
            operation->event->stream_id = stream->id;
            operation->event->stream_position = position;
            operation->event->coverage = next_coverage;
        }
        operation->stream_position = position;
        operation->ordering_id = ordering_id;
        runtime->reserve_accesses(operation->accesses, stream->id, position, &operation->coverage);
        reservations_committed = true;
        const bool was_empty = stream->queue.empty() && !stream->running;
        if (was_empty) {
            runtime->ready.push_back(stream);
            ready_committed = true;
        }
        try {
            stream->queue.push_back(operation);
            queue_committed = true;
        } catch (...) {
            throw;
        }
        if (operation->dependency && !operation->dependency->done) {
            operation->dependency->subscribers.push_back(operation);
            subscriber_committed = true;
            stream->active_barriers.push_back(operation);
            barrier_committed = true;
        }
        stream->coverage.swap(next_coverage);
        ++runtime->outstanding;
        stream->last_accepted = position;
        runtime->next_ordering_id = ordering_id == std::numeric_limits<std::uint64_t>::max() ? 0 : ordering_id + 1;
        if (runtime->metrics_enabled) operation->accepted_ns = detail::steady_now_ns();
        if (was_empty) runtime->ready_cv.notify_one();
    } catch (...) {
        if (barrier_committed) stream->active_barriers.pop_back();
        if (subscriber_committed) operation->dependency->subscribers.pop_back();
        if (queue_committed) stream->queue.pop_back();
        if (ready_committed) runtime->ready.pop_back();
        if (reservations_committed) runtime->release_reservations(*operation, stream->id);
        throw;
    }
}

std::shared_ptr<detail::StagingLease> reserve_staging(
    const std::shared_ptr<detail::RuntimeState>& runtime, std::size_t bytes) {
    auto lease = std::make_shared<detail::StagingLease>(runtime, bytes);
    std::lock_guard lock(runtime->mutex);
    const auto current = runtime->staging_bytes.load(std::memory_order_relaxed);
    if (current > runtime->staging_limit || bytes > runtime->staging_limit - current) {
        throw std::length_error("Runtime staging/result byte limit exceeded; submission was rejected.");
    }
    runtime->staging_bytes.fetch_add(bytes, std::memory_order_relaxed);
    lease->active = true;
    return lease;
}

void run_sync_access(const std::shared_ptr<detail::BufferState>& buffer, detail::AccessMode mode,
                     const std::function<void()>& action) {
    auto runtime = buffer->runtime.lock();
    if (!runtime) throw std::logic_error("Buffer runtime has been destroyed.");
    std::unique_lock buffer_lock(buffer->synchronous_mutex);
    std::uint64_t sync_id;
    {
        std::lock_guard lock(runtime->mutex);
        if (runtime->next_sync_id == 0) throw std::overflow_error("Synchronous access identifier space exhausted.");
        sync_id = runtime->next_sync_id--;
        std::vector<std::pair<std::shared_ptr<detail::BufferState>, detail::AccessMode>> access{{buffer, mode}};
        runtime->reserve_accesses(access, sync_id, 0);
    }
    try {
        action();
    } catch (...) {
        std::lock_guard lock(runtime->mutex);
        detail::Operation operation;
        operation.accesses.push_back({buffer, mode});
        runtime->release_reservations(operation, sync_id);
        throw;
    }
    std::lock_guard lock(runtime->mutex);
    detail::Operation operation;
    operation.accesses.push_back({buffer, mode});
    runtime->release_reservations(operation, sync_id);
}

void mark_download_done(const std::shared_ptr<detail::DownloadState>& result, std::exception_ptr error) noexcept {
    {
        std::lock_guard lock(result->mutex);
        result->error = error;
        result->done = true;
        if (error) result->lease.reset();
    }
    result->cv.notify_all();
}

void synchronize_stream_state(const std::shared_ptr<detail::StreamState>& stream,
                              const std::function<void()>& snapshot_taken = {}) {
    auto runtime = get_runtime(stream);
    std::unique_lock lock(runtime->mutex);
    const auto target = stream->last_accepted;
    lock.unlock();
    if (snapshot_taken) snapshot_taken();
    lock.lock();
    runtime->idle_cv.wait(lock, [&] { return stream->retired >= target; });
    if (stream->failure && stream->failed_at <= target) std::rethrow_exception(stream->failure);
}

} // namespace

Buffer::Buffer() noexcept = default;
Buffer::Buffer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Buffer::~Buffer() = default;
Buffer::Buffer(Buffer&&) noexcept = default;
Buffer& Buffer::operator=(Buffer&&) noexcept = default;

std::size_t Buffer::size_bytes() const {
    if (!impl_ || !impl_->state) throw std::logic_error("Operation on an empty or moved-from GridForge buffer.");
    return impl_->state->bytes;
}

void Buffer::upload(std::span<const std::byte> source, std::size_t offset) {
    if (!impl_ || !impl_->state) throw std::logic_error("Operation on an empty or moved-from GridForge buffer.");
    auto state = impl_->state;
    if (offset > state->bytes || source.size() > state->bytes - offset) throw std::out_of_range("Upload range is outside the buffer.");
    if (source.empty()) return;
    run_sync_access(state, detail::AccessMode::write, [&] {
        if (state->backend == Backend::CPU) {
            std::memcpy(state->cpu_storage.data() + offset, source.data(), source.size());
        }
#if defined(GRIDFORGE_HAS_METAL)
        else state->metal_device->upload(*state->metal_storage, source, offset);
#endif
    });
}

void Buffer::download(std::span<std::byte> destination, std::size_t offset) const {
    if (!impl_ || !impl_->state) throw std::logic_error("Operation on an empty or moved-from GridForge buffer.");
    auto state = impl_->state;
    if (offset > state->bytes || destination.size() > state->bytes - offset) throw std::out_of_range("Download range is outside the buffer.");
    if (destination.empty()) return;
    run_sync_access(state, detail::AccessMode::read, [&] {
        if (state->backend == Backend::CPU) {
            std::memcpy(destination.data(), state->cpu_storage.data() + offset, destination.size());
        }
#if defined(GRIDFORGE_HAS_METAL)
        else state->metal_device->download(*state->metal_storage, destination, offset);
#endif
    });
}

Event::Event() noexcept = default;
Event::Event(std::shared_ptr<detail::EventState> state) noexcept : state_(std::move(state)) {}

bool Event::is_complete() const {
    if (!state_) return true;
    std::lock_guard lock(state_->mutex);
    return state_->done;
}

void Event::wait() const {
    if (!state_) return;
    std::unique_lock lock(state_->mutex);
    state_->cv.wait(lock, [this] { return state_->done; });
    if (state_->error) std::rethrow_exception(state_->error);
}

void Event::rethrow_if_failed() const {
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    if (state_->done && state_->error) std::rethrow_exception(state_->error);
}

DownloadResult::DownloadResult() noexcept = default;
DownloadResult::DownloadResult(std::shared_ptr<detail::DownloadState> state) noexcept : state_(std::move(state)) {}

bool DownloadResult::is_ready() const {
    if (!state_) return true;
    std::lock_guard lock(state_->mutex);
    return state_->done;
}

void DownloadResult::wait() const {
    if (!state_) throw std::logic_error("Wait on an empty GridForge download result.");
    std::unique_lock lock(state_->mutex);
    state_->cv.wait(lock, [this] { return state_->done; });
    if (state_->error) std::rethrow_exception(state_->error);
}

std::span<const std::byte> DownloadResult::get() const {
    wait();
    return state_->bytes;
}

std::size_t DownloadResult::size_bytes() const {
    if (!state_) throw std::logic_error("Operation on an empty GridForge download result.");
    return state_->bytes.size();
}

Stream::Stream() noexcept = default;
Stream::Stream(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

bool Stream::is_closed() const {
    if (!impl_ || !impl_->state) return true;
    auto runtime = impl_->state->runtime.lock();
    if (!runtime) return true;
    std::lock_guard lock(runtime->mutex);
    return impl_->state->closed || !runtime->accepting;
}

void Stream::upload_async(Buffer& destination, std::span<const std::byte> source, std::size_t offset) {
    if (!impl_ || !impl_->state || !destination.impl_ || !destination.impl_->state) throw std::logic_error("Upload requires a live stream and buffer.");
    auto stream = impl_->state;
    auto runtime = get_runtime(stream);
    {
        std::lock_guard lock(runtime->mutex);
        if (!runtime->accepting || stream->closed) throw std::logic_error("Cannot submit work to a closed GridForge stream.");
        if (stream->failure) throw std::logic_error("Cannot submit work to a failed GridForge stream.");
    }
    auto buffer = destination.impl_->state;
    if (buffer->runtime.lock() != runtime) throw std::invalid_argument("Buffer belongs to a different runtime.");
    if (offset > buffer->bytes || source.size() > buffer->bytes - offset) throw std::out_of_range("Upload range is outside the buffer.");
    auto lease = reserve_staging(runtime, source.size());
    std::shared_ptr<std::vector<std::byte>> owned;
    try {
        owned = std::make_shared<std::vector<std::byte>>(source.begin(), source.end());
    } catch (...) {
        lease.reset();
        throw;
    }
    auto operation = std::make_shared<detail::Operation>();
    operation->kind = OperationKind::upload;
    operation->accesses.push_back({buffer, detail::AccessMode::write});
    operation->staging = std::move(lease);
    operation->execute = [buffer, owned, offset] {
        if (owned->empty()) return;
        if (buffer->backend == Backend::CPU) {
            std::memcpy(buffer->cpu_storage.data() + offset, owned->data(), owned->size());
        }
#if defined(GRIDFORGE_HAS_METAL)
        else {
            buffer->metal_device->upload(*buffer->metal_storage, *owned, offset);
        }
#endif
    };
    try {
        accept_operation(runtime, stream, operation);
    } catch (...) {
        operation->staging.reset();
        throw;
    }
}

void Stream::vector_add_async(const Buffer& a, const Buffer& b, Buffer& c, std::size_t element_count) {
    if (!impl_ || !impl_->state || !a.impl_ || !b.impl_ || !c.impl_) throw std::logic_error("Vector addition requires a live stream and buffers.");
    auto stream = impl_->state;
    auto runtime = get_runtime(stream);
    {
        std::lock_guard lock(runtime->mutex);
        if (!runtime->accepting || stream->closed) throw std::logic_error("Cannot submit work to a closed GridForge stream.");
        if (stream->failure) throw std::logic_error("Cannot submit work to a failed GridForge stream.");
    }
    auto left = a.impl_->state;
    auto right = b.impl_->state;
    auto output = c.impl_->state;
    if (left->runtime.lock() != runtime || right->runtime.lock() != runtime || output->runtime.lock() != runtime) throw std::invalid_argument("All vector-add buffers must belong to the stream's runtime.");
    if (left == right || left == output || right == output) throw std::invalid_argument("Vector addition requires distinct A, B, and C buffers.");
    if (element_count > std::numeric_limits<std::size_t>::max() / sizeof(float)) throw std::length_error("Vector element count overflows the required byte size.");
    const auto bytes = element_count * sizeof(float);
    if (left->bytes != bytes || right->bytes != bytes || output->bytes != bytes) throw std::invalid_argument("Vector-add buffers must each be exactly element_count * sizeof(float) bytes.");
    auto operation = std::make_shared<detail::Operation>();
    operation->kind = OperationKind::vector_add;
    operation->accesses = {{left, detail::AccessMode::read}, {right, detail::AccessMode::read}, {output, detail::AccessMode::write}};
    if (runtime->backend == Backend::CPU || element_count == 0) {
        operation->execute = [left, right, output, element_count] {
            if (left->backend == Backend::CPU) {
                const auto* x = reinterpret_cast<const float*>(left->cpu_storage.data());
                const auto* y = reinterpret_cast<const float*>(right->cpu_storage.data());
                auto* z = reinterpret_cast<float*>(output->cpu_storage.data());
                for (std::size_t i = 0; i < element_count; ++i) z[i] = x[i] + y[i];
            }
        };
    }
#if defined(GRIDFORGE_HAS_METAL)
    else {
        operation->submit_async = [left, right, output, element_count](
            detail::CompletionCallback completion) {
            return left->metal_device->submit_vector_add(*left->metal_storage, *right->metal_storage,
                                                         *output->metal_storage, element_count,
                                                         std::move(completion));
        };
    }
#endif
    accept_operation(runtime, stream, operation);
}

DownloadResult Stream::download_async(const Buffer& source, std::size_t offset, std::size_t byte_count) {
    if (!impl_ || !impl_->state || !source.impl_ || !source.impl_->state) throw std::logic_error("Download requires a live stream and buffer.");
    auto stream = impl_->state;
    auto runtime = get_runtime(stream);
    {
        std::lock_guard lock(runtime->mutex);
        if (!runtime->accepting || stream->closed) throw std::logic_error("Cannot submit work to a closed GridForge stream.");
        if (stream->failure) throw std::logic_error("Cannot submit work to a failed GridForge stream.");
    }
    auto buffer = source.impl_->state;
    if (buffer->runtime.lock() != runtime) throw std::invalid_argument("Buffer belongs to a different runtime.");
    if (byte_count == std::dynamic_extent) byte_count = offset <= buffer->bytes ? buffer->bytes - offset : 0;
    if (offset > buffer->bytes || byte_count > buffer->bytes - offset) throw std::out_of_range("Download range is outside the buffer.");

    auto result = std::make_shared<detail::DownloadState>();
    result->lease = reserve_staging(runtime, byte_count);
    try {
        result->bytes.resize(byte_count);
    } catch (...) {
        result->lease.reset();
        throw;
    }
    auto operation = std::make_shared<detail::Operation>();
    operation->kind = OperationKind::download;
    operation->download = result;
    operation->accesses.push_back({buffer, detail::AccessMode::read});
    operation->execute = [buffer, result, offset, byte_count] {
        if (!byte_count) return;
        if (buffer->backend == Backend::CPU) {
            std::memcpy(result->bytes.data(), buffer->cpu_storage.data() + offset, byte_count);
        }
#if defined(GRIDFORGE_HAS_METAL)
        else {
            buffer->metal_device->download(*buffer->metal_storage, result->bytes, offset);
        }
#endif
    };
    try {
        accept_operation(runtime, stream, operation);
    } catch (...) {
        mark_download_done(result, std::current_exception());
        throw;
    }
    return DownloadResult(std::move(result));
}

Event Stream::wait_event(const Event& dependency) {
    if (!impl_ || !impl_->state) throw std::logic_error("Cannot wait on an event from an empty stream.");
    if (!dependency.state_) throw std::invalid_argument("Cannot wait on an invalid or default-constructed event.");
    auto stream = impl_->state;
    auto runtime = get_runtime(stream);
    auto event = std::make_shared<detail::EventState>();
    auto operation = std::make_shared<detail::Operation>();
    operation->kind = OperationKind::event_barrier;
    operation->event = event;
    operation->dependency = dependency.state_;
    operation->execute = [] {};
    {
        std::lock_guard lock(runtime->mutex);
        if (dependency.state_->runtime.lock() != runtime) {
            throw std::invalid_argument("Event dependency must have been recorded by this runtime.");
        }
        if (dependency.state_->operation_id == 0 ||
            !dependency.state_->dependency_eligible ||
            (runtime->next_ordering_id != 0 && dependency.state_->operation_id >= runtime->next_ordering_id)) {
            throw std::invalid_argument("Event dependency must refer to an already accepted event position.");
        }
        if (!runtime->accepting || stream->closed) throw std::logic_error("Cannot submit a dependency to a closed stream.");
        if (stream->failure) throw std::logic_error("Cannot submit work to a failed GridForge stream.");
    }
    try {
        accept_operation(runtime, stream, operation);
    } catch (...) {
        complete_immediate(event, std::current_exception());
        throw;
    }
    return Event(std::move(event));
}

Event Stream::record_event() {
    if (!impl_ || !impl_->state) throw std::logic_error("Cannot record an event on an empty stream.");
    auto stream = impl_->state;
    auto runtime = get_runtime(stream);
    {
        std::lock_guard lock(runtime->mutex);
        if (!runtime->accepting || stream->closed) throw std::logic_error("Cannot submit work to a closed GridForge stream.");
        if (stream->failure) throw std::logic_error("Cannot submit work to a failed GridForge stream.");
    }
    auto event = std::make_shared<detail::EventState>();
    auto operation = std::make_shared<detail::Operation>();
    operation->kind = OperationKind::event_record;
    operation->event = event;
    operation->execute = [] {};
    try {
        accept_operation(runtime, stream, operation);
    } catch (...) {
        complete_immediate(event, std::current_exception());
        throw;
    }
    event->dependency_eligible = true;
    return Event(std::move(event));
}

void Stream::synchronize() const {
    if (!impl_ || !impl_->state) return;
    synchronize_stream_state(impl_->state);
}

Runtime::Runtime(RuntimeOptions options) : impl_(std::make_shared<Impl>(options)) {}
Runtime::~Runtime() {
    if (impl_ && impl_->scheduler) impl_->scheduler->shutdown();
}
Runtime::Runtime(Runtime&& other) noexcept : impl_(std::move(other.impl_)) {}
Runtime& Runtime::operator=(Runtime&& other) noexcept {
    if (this != &other) {
        if (impl_ && impl_->scheduler) impl_->scheduler->shutdown();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Backend Runtime::backend() const {
    if (!impl_) throw std::logic_error("Operation on a moved-from GridForge runtime.");
    return impl_->options.backend;
}
const char* Runtime::backend_name() const {
    switch (backend()) {
    case Backend::CPU: return "CPU";
    case Backend::Metal: return "Metal";
    }
    return "Unknown";
}
const char* Runtime::device_name() const {
    if (!impl_) throw std::logic_error("Operation on a moved-from GridForge runtime.");
    return impl_->device.c_str();
}
std::size_t Runtime::worker_count() const {
    if (!impl_) throw std::logic_error("Operation on a moved-from GridForge runtime.");
    return impl_->options.worker_count;
}

OperationMetricsSnapshot Runtime::drain_operation_metrics() {
    if (!impl_) throw std::logic_error("Operation on a moved-from GridForge runtime.");
    auto scheduler = impl_->scheduler;
    OperationMetricsSnapshot snapshot;
    snapshot.enabled = scheduler->metrics_enabled;
    if (!snapshot.enabled) return snapshot;
    std::lock_guard lock(scheduler->metrics_mutex);
    snapshot.records = scheduler->metrics_records;
    snapshot.dropped_records = scheduler->dropped_metric_records;
    scheduler->metrics_records.clear();
    return snapshot;
}

Buffer Runtime::create_buffer(std::size_t size_bytes) {
    if (!impl_) throw std::logic_error("Operation on a moved-from GridForge runtime.");
    auto core = impl_->scheduler;
    {
        std::lock_guard lock(core->mutex);
        const auto allocated = core->allocated_bytes.load(std::memory_order_relaxed);
        if (allocated > core->allocation_limit || size_bytes > core->allocation_limit - allocated) {
            throw std::length_error("Buffer allocation exceeds the runtime's combined allocation limit.");
        }
        core->allocated_bytes.fetch_add(size_bytes, std::memory_order_relaxed);
    }
    std::shared_ptr<detail::BufferState> state;
    try {
        state = std::make_shared<detail::BufferState>(core, size_bytes);
        state->accounted = true;
#if defined(GRIDFORGE_HAS_METAL)
        if (core->backend == Backend::Metal) state->metal_storage = core->metal->allocate(size_bytes);
#endif
        return Buffer(std::make_unique<Buffer::Impl>(std::move(state)));
    } catch (...) {
        if (!state) core->allocated_bytes.fetch_sub(size_bytes, std::memory_order_relaxed);
        throw;
    }
}

Stream Runtime::create_stream() {
    if (!impl_) throw std::logic_error("Operation on a moved-from GridForge runtime.");
    auto runtime = impl_->scheduler;
    if (runtime->worker_limit == 0) {
        throw std::logic_error("This runtime has no asynchronous execution workers; configure worker_count greater than zero.");
    }
    std::lock_guard lock(runtime->mutex);
    if (!runtime->accepting) throw std::logic_error("Cannot create a stream after runtime shutdown has started.");
    auto state = std::make_shared<detail::StreamState>();
    state->runtime = runtime;
    if (runtime->next_stream_id == 0) throw std::overflow_error("Stream identifier space exhausted.");
    state->id = runtime->next_stream_id++;
    runtime->streams.push_back(state);
    return Stream(std::make_shared<Stream::Impl>(std::move(state)));
}

void Runtime::vector_add(const Buffer& a, const Buffer& b, Buffer& c, std::size_t element_count) {
    if (!impl_ || !a.impl_ || !b.impl_ || !c.impl_) throw std::logic_error("Vector addition requires a live runtime and three live buffers.");
    auto left = a.impl_->state;
    auto right = b.impl_->state;
    auto output = c.impl_->state;
    if (left->runtime.lock() != impl_->scheduler || right->runtime.lock() != impl_->scheduler || output->runtime.lock() != impl_->scheduler) throw std::invalid_argument("All vector-add buffers must belong to the dispatching runtime.");
    if (left == right || left == output || right == output) throw std::invalid_argument("Vector addition requires distinct A, B, and C buffers.");
    if (element_count > std::numeric_limits<std::size_t>::max() / sizeof(float)) throw std::length_error("Vector element count overflows the required byte size.");
    const auto bytes = element_count * sizeof(float);
    if (left->bytes != bytes || right->bytes != bytes || output->bytes != bytes) throw std::invalid_argument("Vector-add buffers must each be exactly element_count * sizeof(float) bytes.");
    if (element_count == 0) return;

    std::scoped_lock buffer_locks(left->synchronous_mutex, right->synchronous_mutex,
                                  output->synchronous_mutex);
    auto scheduler = impl_->scheduler;
    std::vector<std::pair<std::shared_ptr<detail::BufferState>, detail::AccessMode>> accesses{
        {left, detail::AccessMode::read}, {right, detail::AccessMode::read},
        {output, detail::AccessMode::write}};
    std::uint64_t sync_id;
    {
        std::lock_guard lock(scheduler->mutex);
        if (scheduler->next_sync_id == 0) throw std::overflow_error("Synchronous access identifier space exhausted.");
        sync_id = scheduler->next_sync_id--;
            scheduler->reserve_accesses(accesses, sync_id, 0);
    }

    try {
        if (impl_->options.backend == Backend::CPU) {
            if (element_count) {
                const auto* x = reinterpret_cast<const float*>(left->cpu_storage.data());
                const auto* y = reinterpret_cast<const float*>(right->cpu_storage.data());
                auto* z = reinterpret_cast<float*>(output->cpu_storage.data());
                for (std::size_t i = 0; i < element_count; ++i) z[i] = x[i] + y[i];
            }
        }
#if defined(GRIDFORGE_HAS_METAL)
        else {
            scheduler->metal->vector_add(*left->metal_storage, *right->metal_storage,
                                         *output->metal_storage, element_count);
        }
#endif
    } catch (...) {
        std::lock_guard lock(scheduler->mutex);
        detail::Operation operation; operation.accesses = accesses;
        scheduler->release_reservations(operation, sync_id);
        throw;
    }
    std::lock_guard lock(scheduler->mutex);
    detail::Operation operation; operation.accesses = accesses;
    scheduler->release_reservations(operation, sync_id);
}

void Runtime::synchronize() {
    if (!impl_) throw std::logic_error("Operation on a moved-from GridForge runtime.");
    auto runtime = impl_->scheduler;
    std::unique_lock lock(runtime->mutex);
    std::vector<std::pair<std::shared_ptr<detail::StreamState>, std::size_t>> snapshot;
    for (auto& stream : runtime->streams) snapshot.emplace_back(stream, stream->last_accepted);
    runtime->idle_cv.wait(lock, [&] {
        return std::all_of(snapshot.begin(), snapshot.end(), [](const auto& item) {
            return item.first->retired >= item.second;
        });
    });
    for (const auto& [stream, target] : snapshot) {
        if (stream->failure && stream->failed_at <= target) std::rethrow_exception(stream->failure);
    }
}

namespace detail {
Event TestAccess::enqueue_test_task(Stream& stream, std::function<void()> task) {
    if (!stream.impl_ || !stream.impl_->state) throw std::logic_error("Test task requires a live stream.");
    auto state = stream.impl_->state;
    auto runtime = get_runtime(state);
    auto operation = std::make_shared<Operation>();
    operation->kind = OperationKind::test_task;
    operation->execute = std::move(task);
    operation->event = std::make_shared<EventState>();
    accept_operation(runtime, state, operation);
    return Event(operation->event);
}

Event TestAccess::enqueue_deferred_task(
    Stream& stream, std::function<std::shared_ptr<void>(std::function<void(std::exception_ptr)>)> submit) {
    if (!stream.impl_ || !stream.impl_->state) throw std::logic_error("Test task requires a live stream.");
    auto state = stream.impl_->state;
    auto runtime = get_runtime(state);
    auto operation = std::make_shared<Operation>();
    operation->kind = OperationKind::test_task;
    operation->submit_async = [submit = std::move(submit)](CompletionCallback completion) mutable {
        return submit([completion = std::move(completion)](std::exception_ptr error) mutable {
            completion(std::move(error), std::nullopt, std::nullopt);
        });
    };
    operation->event = std::make_shared<EventState>();
    accept_operation(runtime, state, operation);
    return Event(operation->event);
}

void TestAccess::synchronize_after_snapshot(Stream& stream, std::function<void()> snapshot_taken) {
    if (!stream.impl_ || !stream.impl_->state) throw std::logic_error("Test synchronization requires a live stream.");
    synchronize_stream_state(stream.impl_->state, snapshot_taken);
}
} // namespace detail

} // namespace gridforge
