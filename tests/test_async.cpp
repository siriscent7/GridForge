#include "gridforge/gridforge.hpp"
#include "test_hooks.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <latch>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Exception, class Function>
void require_throws(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

bool close_enough(float actual, float expected) {
    constexpr float absolute_tolerance = 1.0e-5F;
    constexpr float relative_tolerance = 1.0e-5F;
    return std::isfinite(actual) && std::isfinite(expected) &&
           std::abs(actual - expected) <= absolute_tolerance + relative_tolerance * std::abs(expected);
}

std::vector<float> floats_from(const gridforge::DownloadResult& result) {
    result.wait();
    require(result.size_bytes() % sizeof(float) == 0, "download result size is not float aligned");
    std::vector<float> values(result.size_bytes() / sizeof(float));
    if (!values.empty()) std::memcpy(values.data(), result.get().data(), result.size_bytes());
    return values;
}

void test_ordering_known_and_randomized() {
    gridforge::Runtime runtime;
    auto stream = runtime.create_stream();
    const std::array<float, 4> a{-3.0F, 0.0F, 1.25F, -100.0F};
    const std::array<float, 4> b{2.0F, -5.0F, 8.0F, 99.5F};
    constexpr std::array<float, 4> expected{-1.0F, -5.0F, 9.25F, -0.5F};
    auto buffer_a = runtime.create_buffer(sizeof(a));
    auto buffer_b = runtime.create_buffer(sizeof(b));
    auto buffer_c = runtime.create_buffer(sizeof(expected));
    stream.upload_async(buffer_a, std::as_bytes(std::span(a)));
    stream.upload_async(buffer_b, std::as_bytes(std::span(b)));
    stream.vector_add_async(buffer_a, buffer_b, buffer_c, a.size());
    auto result = stream.download_async(buffer_c);
    const auto values = floats_from(result);
    for (std::size_t i = 0; i < values.size(); ++i) require(close_enough(values[i], expected[i]), "independent known answer mismatch");

    std::mt19937 generator(0x51A7U);
    std::uniform_real_distribution<float> distribution(-200.0F, 200.0F);
    for (const std::size_t count : {0U, 1U, 7U, 255U, 256U, 257U, 1021U, 4097U}) {
        std::vector<float> x(count), y(count), reference(count);
        for (std::size_t i = 0; i < count; ++i) {
            x[i] = distribution(generator);
            y[i] = distribution(generator);
            reference[i] = x[i] + y[i];
        }
        auto left = runtime.create_buffer(count * sizeof(float));
        auto right = runtime.create_buffer(count * sizeof(float));
        auto output = runtime.create_buffer(count * sizeof(float));
        stream.upload_async(left, std::as_bytes(std::span<const float>(x)));
        stream.upload_async(right, std::as_bytes(std::span<const float>(y)));
        stream.vector_add_async(left, right, output, count);
        auto actual = floats_from(stream.download_async(output));
        for (std::size_t i = 0; i < count; ++i) require(close_enough(actual[i], reference[i]), "randomized CPU comparison mismatch");
    }
}

void test_event_position_and_empty_stream() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1});
    auto stream = runtime.create_stream();
    auto empty = stream.record_event();
    empty.wait();
    require(empty.is_complete(), "empty stream event should complete");
    stream.synchronize();

    std::latch first_entered(1);
    std::latch release_first(1);
    auto first = gridforge::detail::TestAccess::enqueue_test_task(stream, [&] {
        first_entered.count_down();
        release_first.wait();
    });
    first_entered.wait();
    auto marker = stream.record_event();
    std::latch later_entered(1);
    std::latch release_later(1);
    auto later = gridforge::detail::TestAccess::enqueue_test_task(stream, [&] {
        later_entered.count_down();
        release_later.wait();
    });
    release_first.count_down();
    later_entered.wait();
    marker.wait();
    require(marker.is_complete(), "event must complete before a later blocked submission");
    require(!later.is_complete(), "later operation should still be blocked after earlier event completion");
    release_later.count_down();
    later.wait();
}

void test_upload_staging_download_ownership_and_reuse() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 2});
    auto stream = runtime.create_stream();
    constexpr std::size_t count = 257;
    auto a = runtime.create_buffer(count * sizeof(float));
    auto b = runtime.create_buffer(count * sizeof(float));
    auto c = runtime.create_buffer(count * sizeof(float));

    std::vector<float> source_a(count), source_b(count), expected(count);
    for (std::size_t i = 0; i < count; ++i) {
        source_a[i] = static_cast<float>(static_cast<int>(i % 41) - 20);
        source_b[i] = -static_cast<float>(i % 19);
        expected[i] = source_a[i] + source_b[i];
    }
    stream.upload_async(a, std::as_bytes(std::span<const float>(source_a)));
    stream.upload_async(b, std::as_bytes(std::span<const float>(source_b)));
    std::fill(source_a.begin(), source_a.end(), 999.0F);
    std::fill(source_b.begin(), source_b.end(), -999.0F);
    stream.vector_add_async(a, b, c, count);
    auto result = stream.download_async(c);
    auto actual = floats_from(result);
    for (std::size_t i = 0; i < count; ++i) require(close_enough(actual[i], expected[i]), "upload source was not copied into staging before return");

    for (int repetition = 0; repetition < 5; ++repetition) {
        std::vector<float> x(count, static_cast<float>(repetition - 2));
        std::vector<float> y(count, static_cast<float>(7 - repetition));
        stream.upload_async(a, std::as_bytes(std::span<const float>(x)));
        stream.upload_async(b, std::as_bytes(std::span<const float>(y)));
        stream.vector_add_async(a, b, c, count);
        auto repeated = floats_from(stream.download_async(c));
        for (float value : repeated) require(close_enough(value, 5.0F), "same-stream buffer reuse failed");
    }
}

void test_handle_lifetimes_zero_work_and_more_streams_than_workers() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 2});
    {
        auto stream = runtime.create_stream();
        auto empty_a = runtime.create_buffer(0);
        auto empty_b = runtime.create_buffer(0);
        auto empty_c = runtime.create_buffer(0);
        stream.upload_async(empty_a, {});
        stream.vector_add_async(empty_a, empty_b, empty_c, 0);
        auto empty_result = stream.download_async(empty_c);
        empty_result.wait();
        require(empty_result.size_bytes() == 0, "zero-length download should own an empty result");
        stream.synchronize();
    }

    auto retained_stream = runtime.create_stream();
    std::latch entered(1);
    std::latch release(1);
    auto gate = gridforge::detail::TestAccess::enqueue_test_task(retained_stream, [&] {
        entered.count_down();
        release.wait();
    });
    entered.wait();
    {
        auto left = runtime.create_buffer(sizeof(float));
        auto right = runtime.create_buffer(sizeof(float));
        auto output = runtime.create_buffer(sizeof(float));
        const float x = -4.0F;
        const float y = 1.5F;
        retained_stream.upload_async(left, std::as_bytes(std::span(&x, 1)));
        retained_stream.upload_async(right, std::as_bytes(std::span(&y, 1)));
        retained_stream.vector_add_async(left, right, output, 1);
        // Public handles are destroyed here; accepted operations retain their storage.
    }
    auto completion = retained_stream.record_event();

    std::vector<gridforge::Stream> streams;
    for (int i = 0; i < 12; ++i) streams.push_back(runtime.create_stream());
    std::vector<gridforge::Event> events;
    for (auto& stream : streams) events.push_back(gridforge::detail::TestAccess::enqueue_test_task(stream, [] {}));
    release.count_down();
    gate.wait();
    completion.wait();
    for (const auto& event : events) event.wait();
}

void test_access_conflicts() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1});
    auto first = runtime.create_stream();
    auto second = runtime.create_stream();
    auto buffer = runtime.create_buffer(sizeof(float));
    std::latch entered(1);
    std::latch release(1);
    auto gate = gridforge::detail::TestAccess::enqueue_test_task(first, [&] {
        entered.count_down();
        release.wait();
    });
    entered.wait();
    const float value = 12.0F;
    first.upload_async(buffer, std::as_bytes(std::span(&value, 1)));
    require_throws<std::logic_error>([&] { second.upload_async(buffer, std::as_bytes(std::span(&value, 1))); },
                                     "conflicting cross-stream write must be rejected");
    require_throws<std::logic_error>([&] { buffer.upload(std::as_bytes(std::span(&value, 1))); },
                                     "conflicting synchronous write must be rejected");
    std::array<std::byte, sizeof(float)> destination{};
    require_throws<std::logic_error>([&] { buffer.download(destination); },
                                     "conflicting synchronous read must be rejected");
    release.count_down();
    gate.wait();
    first.synchronize();
    second.upload_async(buffer, std::as_bytes(std::span(&value, 1)));
    second.synchronize();
}

void test_invalid_async_submissions() {
    gridforge::Runtime runtime;
    auto stream = runtime.create_stream();
    auto one = runtime.create_buffer(sizeof(float));
    auto two = runtime.create_buffer(2 * sizeof(float));
    auto output = runtime.create_buffer(sizeof(float));
    require_throws<std::invalid_argument>([&] { stream.vector_add_async(one, two, output, 1); },
                                          "mismatched async vector-add sizes must be rejected");
    require_throws<std::length_error>([&] {
        stream.vector_add_async(one, two, output, static_cast<std::size_t>(-1));
    }, "async vector size multiplication overflow must be rejected");
    gridforge::Runtime other;
    auto foreign = other.create_buffer(sizeof(float));
    require_throws<std::invalid_argument>([&] { stream.upload_async(foreign, std::span<const std::byte>{}); },
                                          "foreign runtime buffers must be rejected");
    auto usable = stream.record_event();
    usable.wait();
}

void test_concurrent_submitters_and_event_observers() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 3});
    auto stream = runtime.create_stream();
    constexpr std::size_t submitters = 8;
    constexpr std::size_t operations_per_submitter = 16;
    std::barrier start(static_cast<std::ptrdiff_t>(submitters));
    std::vector<std::vector<gridforge::Event>> events(submitters);
    std::vector<std::thread> submit_threads;
    for (std::size_t t = 0; t < submitters; ++t) {
        submit_threads.emplace_back([&, t] {
            start.arrive_and_wait();
            for (std::size_t i = 0; i < operations_per_submitter; ++i) {
                events[t].push_back(gridforge::detail::TestAccess::enqueue_test_task(stream, [] {}));
            }
        });
    }
    for (auto& thread : submit_threads) thread.join();
    auto marker = stream.record_event();
    std::atomic<bool> observer_failed{false};
    std::vector<std::thread> observers;
    for (std::size_t t = 0; t < 8; ++t) {
        observers.emplace_back([&] {
            try {
                marker.wait();
                if (!marker.is_complete()) observer_failed.store(true);
                for (const auto& row : events) for (const auto& event : row) event.rethrow_if_failed();
            } catch (...) {
                observer_failed.store(true);
            }
        });
    }
    for (auto& observer : observers) observer.join();
    require(!observer_failed.load(), "concurrent event observer failed");
    for (const auto& row : events) for (const auto& event : row) event.wait();
}

void test_operation_and_staging_limits_recover() {
    gridforge::Runtime operation_limited({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1, 1, 64});
    auto stream = operation_limited.create_stream();
    std::latch entered(1);
    std::latch release(1);
    auto blocked = gridforge::detail::TestAccess::enqueue_test_task(stream, [&] {
        entered.count_down();
        release.wait();
    });
    entered.wait();
    require_throws<std::length_error>([&] { (void)stream.record_event(); }, "running operation must consume outstanding slot");
    release.count_down();
    blocked.wait();
    auto recovered = stream.record_event();
    recovered.wait();

    gridforge::Runtime staging_limited({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1, 16, 8});
    auto staging_stream = staging_limited.create_stream();
    auto bytes = staging_limited.create_buffer(8);
    std::latch stage_entered(1);
    std::latch stage_release(1);
    auto stage_gate = gridforge::detail::TestAccess::enqueue_test_task(staging_stream, [&] {
        stage_entered.count_down();
        stage_release.wait();
    });
    stage_entered.wait();
    std::array<std::byte, 8> source{};
    staging_stream.upload_async(bytes, source);
    require_throws<std::length_error>([&] { staging_stream.upload_async(bytes, std::span<const std::byte>(source).first(1)); },
                                      "staging cap must reject excess queued upload bytes");
    stage_release.count_down();
    stage_gate.wait();
    staging_stream.synchronize();

    auto retained_result = staging_stream.download_async(bytes);
    retained_result.wait();
    require_throws<std::length_error>([&] { (void)staging_stream.download_async(bytes, 0, 1); },
                                      "completed retained download must remain charged to staging cap");
    retained_result = gridforge::DownloadResult{};
    auto after_release = staging_stream.download_async(bytes, 0, 1);
    after_release.wait();
}

void test_failure_propagation_and_other_stream_progress() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 2});
    auto failed_stream = runtime.create_stream();
    auto healthy_stream = runtime.create_stream();
    std::latch entered(1);
    std::latch release(1);
    auto failed = gridforge::detail::TestAccess::enqueue_test_task(failed_stream, [&] {
        entered.count_down();
        release.wait();
        throw std::runtime_error("injected worker failure");
    });
    entered.wait();
    auto later = failed_stream.record_event();
    auto healthy = healthy_stream.record_event();
    healthy.wait();
    release.count_down();
    require_throws<std::runtime_error>([&] { failed.wait(); }, "failed operation event must rethrow execution error");
    require_throws<std::runtime_error>([&] { later.wait(); }, "later accepted event must inherit stream failure");
    require_throws<std::runtime_error>([&] { failed_stream.synchronize(); }, "stream synchronization must propagate execution failure");
    require_throws<std::logic_error>([&] { (void)failed_stream.record_event(); }, "failed stream must reject new work");
    require(healthy.is_complete(), "unaffected stream should progress despite another stream failure");
    auto after_failure = healthy_stream.record_event();
    after_failure.wait();
    failed_stream = gridforge::Stream{};
    require_throws<std::runtime_error>([&] { runtime.synchronize(); },
                                       "runtime synchronization must retain and report completed stream failures");
}

void test_runtime_shutdown_drains_and_closes_streams() {
    auto runtime = std::make_unique<gridforge::Runtime>(gridforge::RuntimeOptions{gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 2});
    auto stream = runtime->create_stream();
    std::latch entered(1);
    std::latch release(1);
    auto event = gridforge::detail::TestAccess::enqueue_test_task(stream, [&] {
        entered.count_down();
        release.wait();
    });
    entered.wait();
    std::latch destructor_started(1);
    std::thread destroyer([runtime = std::move(runtime), &destructor_started]() mutable {
        destructor_started.count_down();
        runtime.reset();
    });
    destructor_started.wait();
    while (!stream.is_closed()) std::this_thread::yield();
    release.count_down();
    destroyer.join();
    event.wait();
    require(stream.is_closed(), "stream handle must be closed after runtime destruction");
    require_throws<std::logic_error>([&] { (void)stream.record_event(); }, "closed stream must reject new submissions");
}

void test_deferred_submission_does_not_block_worker_and_retires_exactly() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1});
    auto submitted_stream = runtime.create_stream();
    auto independent_stream = runtime.create_stream();
    std::mutex mutex;
    std::condition_variable cv;
    std::function<void(std::exception_ptr)> complete;
    bool callback_registered = false;
    auto deferred = gridforge::detail::TestAccess::enqueue_deferred_task(submitted_stream,
        [&](auto callback) {
            {
                std::lock_guard lock(mutex);
                complete = std::move(callback);
                callback_registered = true;
            }
            cv.notify_all();
            return std::shared_ptr<void>{};
        });
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return callback_registered; });
    }
    auto position = submitted_stream.record_event();
    require(!deferred.is_complete() && !position.is_complete(),
            "deferred operation/event must remain pending until callback completion");
    auto independent = gridforge::detail::TestAccess::enqueue_test_task(independent_stream, [] {});
    independent.wait(); // With one host worker, this proves the deferred operation did not occupy it.
    {
        std::lock_guard lock(mutex);
        complete({});
    }
    deferred.wait();
    position.wait();
    require(position.is_complete(), "event position should retire after deferred completion");
}

void test_deferred_operation_counts_until_retirement() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes,
                                1, 1, gridforge::default_max_staging_bytes});
    auto stream = runtime.create_stream();
    std::mutex mutex;
    std::condition_variable cv;
    std::function<void(std::exception_ptr)> complete;
    bool submitted = false;
    auto event = gridforge::detail::TestAccess::enqueue_deferred_task(stream, [&](auto callback) {
        {
            std::lock_guard lock(mutex);
            complete = std::move(callback);
            submitted = true;
        }
        cv.notify_all();
        return std::shared_ptr<void>{};
    });
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return submitted; });
    }
    require_throws<std::length_error>([&] { (void)stream.record_event(); },
                                      "submitted but unretired operation must consume the outstanding slot");
    complete({});
    event.wait();
    auto recovered = stream.record_event();
    recovered.wait();
}

void test_stream_synchronization_uses_acceptance_snapshot() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1});
    auto stream = runtime.create_stream();
    std::mutex mutex;
    std::condition_variable cv;
    std::function<void(std::exception_ptr)> finish_first;
    std::function<void(std::exception_ptr)> finish_later;
    bool first_registered = false;
    bool later_registered = false;
    auto first = gridforge::detail::TestAccess::enqueue_deferred_task(stream, [&](auto callback) {
        {
            std::lock_guard lock(mutex);
            finish_first = std::move(callback);
            first_registered = true;
        }
        cv.notify_all();
        return std::shared_ptr<void>{};
    });
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return first_registered; });
    }
    std::latch snapshot_taken(1);
    std::latch synchronize_returned(1);
    std::thread synchronizer([&] {
        gridforge::detail::TestAccess::synchronize_after_snapshot(stream, [&] { snapshot_taken.count_down(); });
        synchronize_returned.count_down();
    });
    snapshot_taken.wait();
    auto later = gridforge::detail::TestAccess::enqueue_deferred_task(stream, [&](auto callback) {
        {
            std::lock_guard lock(mutex);
            finish_later = std::move(callback);
            later_registered = true;
        }
        cv.notify_all();
        return std::shared_ptr<void>{};
    });
    finish_first({});
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return later_registered; });
    }
    synchronize_returned.wait();
    require(!later.is_complete(), "later operation must not extend an earlier synchronize snapshot");
    finish_later({});
    first.wait();
    later.wait();
    synchronizer.join();
}

void test_deferred_failure_and_shutdown_completion() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1});
    auto failed_stream = runtime.create_stream();
    std::mutex failure_mutex;
    std::condition_variable failure_cv;
    std::function<void(std::exception_ptr)> finish_failure;
    auto failed = gridforge::detail::TestAccess::enqueue_deferred_task(failed_stream,
        [&](auto callback) {
            {
                std::lock_guard lock(failure_mutex);
                finish_failure = std::move(callback);
            }
            failure_cv.notify_all();
            return std::shared_ptr<void>{};
        });
    auto skipped = failed_stream.record_event();
    {
        std::unique_lock lock(failure_mutex);
        failure_cv.wait(lock, [&] { return static_cast<bool>(finish_failure); });
    }
    finish_failure(std::make_exception_ptr(std::runtime_error("injected simulated Metal completion error")));
    require_throws<std::runtime_error>([&] { failed.wait(); }, "deferred completion error was not delivered");
    require_throws<std::runtime_error>([&] { skipped.wait(); }, "later work was not failed after deferred error");

    auto submit_failure_stream = runtime.create_stream();
    auto submit_failure = gridforge::detail::TestAccess::enqueue_deferred_task(
        submit_failure_stream, [](auto) -> std::shared_ptr<void> { throw std::runtime_error("simulated command encoding failure"); });
    require_throws<std::runtime_error>([&] { submit_failure.wait(); },
                                       "submission/encoding failure was not retired on its stream");

    auto owned_runtime = std::make_unique<gridforge::Runtime>(gridforge::RuntimeOptions{gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1});
    auto stream = owned_runtime->create_stream();
    std::mutex mutex;
    std::condition_variable cv;
    std::function<void(std::exception_ptr)> finish;
    bool registered = false;
    auto event = gridforge::detail::TestAccess::enqueue_deferred_task(stream, [&](auto callback) {
        {
            std::lock_guard lock(mutex);
            finish = std::move(callback);
            registered = true;
        }
        cv.notify_all();
        return std::shared_ptr<void>{};
    });
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return registered; });
    }
    std::latch destructor_started(1);
    std::thread destroyer([runtime = std::move(owned_runtime), &destructor_started]() mutable {
        destructor_started.count_down();
        runtime.reset();
    });
    destructor_started.wait();
    while (!stream.is_closed()) std::this_thread::yield();
    finish({});
    destroyer.join();
    event.wait();
}

void test_operation_metrics_disabled_bounded_drain_and_status() {
    {
        gridforge::Runtime runtime;
        auto stream = runtime.create_stream();
        auto event = gridforge::detail::TestAccess::enqueue_test_task(stream, [] {});
        event.wait();
        const auto metrics = runtime.drain_operation_metrics();
        require(!metrics.enabled && metrics.records.empty() && metrics.dropped_records == 0,
                "operation metrics must be disabled by default");
    }

    {
        gridforge::RuntimeOptions options;
        options.backend = gridforge::Backend::CPU;
        options.enable_operation_metrics = true;
        options.operation_metrics_capacity = 2;
        gridforge::Runtime runtime(options);
        auto stream = runtime.create_stream();
        auto first = gridforge::detail::TestAccess::enqueue_test_task(stream, [] {});
        auto second = gridforge::detail::TestAccess::enqueue_test_task(stream, [] {});
        auto third = gridforge::detail::TestAccess::enqueue_test_task(stream, [] {});
        first.wait();
        second.wait();
        third.wait();
        auto drained = runtime.drain_operation_metrics();
        require(drained.enabled && drained.records.size() == 2 && drained.dropped_records == 1,
                "bounded metrics must drop excess records and report the count");
        require(drained.records[0].accepted_steady_ns && drained.records[0].execution_start_steady_ns &&
                drained.records[0].host_completion_steady_ns && drained.records[0].retirement_publication_steady_ns,
                "CPU lifecycle timestamps were not captured");
        const auto empty = runtime.drain_operation_metrics();
        require(empty.records.empty() && empty.dropped_records == 1,
                "draining metrics must clear records and retain cumulative drop accounting");
    }

    {
        gridforge::RuntimeOptions options;
        options.backend = gridforge::Backend::CPU;
        options.enable_operation_metrics = true;
        options.operation_metrics_capacity = 8;
        gridforge::Runtime runtime(options);
        auto stream = runtime.create_stream();
        std::mutex mutex;
        std::condition_variable cv;
        std::function<void(std::exception_ptr)> complete;
        bool registered = false;
        auto failed = gridforge::detail::TestAccess::enqueue_deferred_task(stream, [&](auto callback) {
            {
                std::lock_guard lock(mutex);
                complete = std::move(callback);
                registered = true;
            }
            cv.notify_all();
            return std::shared_ptr<void>{};
        });
        auto skipped = gridforge::detail::TestAccess::enqueue_test_task(stream, [] {});
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return registered; });
        }
        complete(std::make_exception_ptr(std::runtime_error("metrics injected failure")));
        require_throws<std::runtime_error>([&] { failed.wait(); }, "instrumented failure was not propagated");
        require_throws<std::runtime_error>([&] { skipped.wait(); }, "instrumented skipped operation did not inherit failure");
        const auto metrics = runtime.drain_operation_metrics();
        require(metrics.records.size() == 2 && metrics.records[0].status == gridforge::OperationStatus::failed &&
                metrics.records[1].status == gridforge::OperationStatus::skipped,
                "failed and skipped operation metrics statuses are inaccurate");
        require(metrics.records[0].operation_id != 0 && metrics.records[0].stream_id != 0,
                "operation metrics omitted accepted operation/stream IDs");
    }
}

void test_completion_callback_before_submit_return_handshake() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1});
    auto stream = runtime.create_stream();
    std::latch callback_forwarded(1);
    std::latch allow_submission_return(1);
    auto event = gridforge::detail::TestAccess::enqueue_deferred_task(stream, [&](auto callback) {
        callback({});
        callback_forwarded.count_down();
        allow_submission_return.wait();
        return std::shared_ptr<void>{};
    });
    callback_forwarded.wait();
    require(!event.is_complete(), "early completion callback must not retire before submission returns");
    allow_submission_return.count_down();
    event.wait();
}

void run_all_tests() {
    test_ordering_known_and_randomized();
    test_event_position_and_empty_stream();
    test_upload_staging_download_ownership_and_reuse();
    test_handle_lifetimes_zero_work_and_more_streams_than_workers();
    test_access_conflicts();
    test_invalid_async_submissions();
    test_concurrent_submitters_and_event_observers();
    test_operation_and_staging_limits_recover();
    test_failure_propagation_and_other_stream_progress();
    test_runtime_shutdown_drains_and_closes_streams();
    test_deferred_submission_does_not_block_worker_and_retires_exactly();
    test_deferred_operation_counts_until_retirement();
    test_stream_synchronization_uses_acceptance_snapshot();
    test_deferred_failure_and_shutdown_completion();
    test_operation_metrics_disabled_bounded_drain_and_status();
    test_completion_callback_before_submit_return_handshake();
}

} // namespace

int main() {
    try {
        run_all_tests();
        std::cout << "PASS: CPU asynchronous stream and event tests\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
