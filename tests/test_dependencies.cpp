#include "gridforge/gridforge.hpp"
#include "test_hooks.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

template<class E, class F>
void require_throws(F&& fn, const std::string& message) {
    try { fn(); } catch (const E&) { return; }
    throw std::runtime_error(message);
}

bool near(float got, float expected) {
    constexpr float abs_tol = 1.0e-5F;
    constexpr float rel_tol = 1.0e-5F;
    return std::isfinite(got) && std::isfinite(expected) &&
        std::abs(got - expected) <= abs_tol + rel_tol * std::abs(expected);
}

std::vector<float> get_floats(const gridforge::DownloadResult& result) {
    const auto bytes = result.get();
    require(bytes.size() % sizeof(float) == 0, "result has invalid float byte count");
    std::vector<float> values(bytes.size() / sizeof(float));
    if (!values.empty()) std::memcpy(values.data(), bytes.data(), bytes.size());
    return values;
}

struct Deferred {
    std::mutex mutex;
    std::condition_variable cv;
    std::function<void(std::exception_ptr)> finish;
    bool registered{false};

    gridforge::Event enqueue(gridforge::Stream& stream) {
        return gridforge::detail::TestAccess::enqueue_deferred_task(stream, [this](auto completion) -> std::shared_ptr<void> {
            {
                std::lock_guard lock(mutex);
                finish = std::move(completion);
                registered = true;
            }
            cv.notify_all();
            return {};
        });
    }

    void wait_registered() {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return registered; });
    }

    void complete(std::exception_ptr error = {}) {
        std::function<void(std::exception_ptr)> callback;
        {
            std::lock_guard lock(mutex);
            callback = finish;
        }
        require(static_cast<bool>(callback), "deferred callback not registered");
        callback(std::move(error));
    }
};

void test_three_stream_transitive_handoff_single_worker() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1});
    auto producer = runtime.create_stream();
    auto intermediate = runtime.create_stream();
    auto consumer = runtime.create_stream();
    constexpr std::size_t n = 257;
    std::vector<float> a(n), b(n), expected(n);
    std::mt19937 rng(0xD31U);
    std::uniform_real_distribution<float> dist(-20.0F, 20.0F);
    for (std::size_t i = 0; i < n; ++i) {
        a[i] = dist(rng);
        b[i] = dist(rng);
        expected[i] = a[i] + 2.0F * b[i];
    }
    auto ba = runtime.create_buffer(n * sizeof(float));
    auto bb = runtime.create_buffer(n * sizeof(float));
    auto bc = runtime.create_buffer(n * sizeof(float));
    auto bd = runtime.create_buffer(n * sizeof(float));

    Deferred gate;
    (void)gate.enqueue(producer);
    gate.wait_registered();
    producer.upload_async(ba, std::as_bytes(std::span<const float>(a)));
    producer.upload_async(bb, std::as_bytes(std::span<const float>(b)));
    producer.vector_add_async(ba, bb, bc, n);
    auto e1 = producer.record_event();

    auto e1_wait = intermediate.wait_event(e1);
    intermediate.vector_add_async(bc, bb, bd, n);
    auto e2 = intermediate.record_event();

    // A second consumer can register the same still-pending event.
    auto second_consumer = runtime.create_stream();
    auto e1_copy = second_consumer.wait_event(e1);
    auto c_copy = second_consumer.download_async(bc);

    auto e2_wait = consumer.wait_event(e2);
    auto result = consumer.download_async(bd);
    auto done = consumer.record_event();
    require(!done.is_complete(), "final event must remain pending until producer dependency completes");

    // While the single host worker handles the blocked streams, completing the source task
    // lets the producer continue; a blocked consumer itself consumes no worker.
    gate.complete();
    done.wait();
    e1_wait.wait();
    e2_wait.wait();
    e1_copy.wait();
    auto actual = get_floats(result);
    auto c_values = get_floats(c_copy);
    for (std::size_t i = 0; i < n; ++i) {
        require(near(actual[i], expected[i]), "transitive CPU handoff result mismatch");
        require(near(c_values[i], a[i] + b[i]), "multiple consumer event handoff mismatch");
    }
}

void test_completed_and_self_dependencies_and_multiple_barriers() {
    gridforge::Runtime runtime;
    auto stream = runtime.create_stream();
    auto before = stream.record_event();
    before.wait();
    auto after = stream.wait_event(before);
    auto after2 = stream.wait_event(before);
    auto marker = stream.record_event();
    marker.wait();
    after.wait();
    after2.wait();
        require_throws<std::invalid_argument>([&] { (void)stream.wait_event(after); },
            "barrier completion event must not be treated as a record_event placeholder");

    auto self_position = stream.record_event();
    auto self_barrier = stream.wait_event(self_position);
    auto self_after = stream.record_event();
    self_after.wait();
    self_barrier.wait();
}

void test_blocked_consumer_does_not_occupy_only_worker() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1});
    auto producer = runtime.create_stream();
    auto consumer = runtime.create_stream();
    Deferred source_gate;
    (void)source_gate.enqueue(producer);
    source_gate.wait_registered();
    auto dependency = producer.record_event();
    auto barrier = consumer.wait_event(dependency);
    std::latch producer_progress(1);
    auto progress = gridforge::detail::TestAccess::enqueue_test_task(producer, [&] {
        producer_progress.count_down();
    });
    source_gate.complete();
    producer_progress.wait();
    progress.wait();
    barrier.wait();
}

void test_prefix_coverage_and_unordered_conflicts() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1});
    auto producer = runtime.create_stream();
    auto consumer = runtime.create_stream();
    auto shared = runtime.create_buffer(sizeof(float));
    auto input_b = runtime.create_buffer(sizeof(float));
    auto output = runtime.create_buffer(sizeof(float));
    const float one = 1.0F, two = 2.0F;

    Deferred gate;
    (void)gate.enqueue(producer);
    gate.wait_registered();
    producer.upload_async(shared, std::as_bytes(std::span(&one, 1)));
    auto prefix = producer.record_event();
    // This later write is deliberately outside the recorded prefix.
    producer.upload_async(shared, std::as_bytes(std::span(&two, 1)));
    consumer.upload_async(input_b, std::as_bytes(std::span(&one, 1)));
    (void)consumer.wait_event(prefix);
    require_throws<std::logic_error>([&] { consumer.vector_add_async(shared, input_b, output, 1); },
        "event prefix incorrectly covered a later producer write");
    gate.complete();
    prefix.wait();
    producer.synchronize();
    consumer.synchronize();

    // Unrelated work is still a conflict without an ordering edge while that work is still pending.
    auto other_writer = runtime.create_stream();
    auto unrelated = runtime.create_buffer(sizeof(float));
    Deferred unrelated_gate;
    (void)unrelated_gate.enqueue(other_writer);
    unrelated_gate.wait_registered();
    other_writer.upload_async(unrelated, std::as_bytes(std::span(&one, 1)));
    require_throws<std::logic_error>([&] { consumer.upload_async(unrelated, std::as_bytes(std::span(&two, 1))); },
        "unordered cross-stream buffer conflict was accepted");
    unrelated_gate.complete();
    other_writer.synchronize();
}

void test_read_read_sharing_and_failed_dependency() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 2});
    auto seed = runtime.create_stream();
    auto data = runtime.create_buffer(sizeof(float));
    const float value = -3.0F;
    seed.upload_async(data, std::as_bytes(std::span(&value, 1)));
    auto ready = seed.record_event();
    ready.wait();
    auto reader_a = runtime.create_stream();
    auto reader_b = runtime.create_stream();
    (void)reader_a.wait_event(ready);
    (void)reader_b.wait_event(ready);
    Deferred read_gate;
    (void)read_gate.enqueue(reader_a);
    read_gate.wait_registered();
    auto first = reader_a.download_async(data);
    auto second = reader_b.download_async(data);
    read_gate.complete();
    require(near(get_floats(first)[0], value), "first read/read consumer mismatch");
    require(near(get_floats(second)[0], value), "second read/read consumer mismatch");

    auto broken_stream = runtime.create_stream();
    auto consumer = runtime.create_stream();
    Deferred broken_gate;
    (void)broken_gate.enqueue(broken_stream);
    broken_gate.wait_registered();
    auto broken = gridforge::detail::TestAccess::enqueue_test_task(broken_stream, [] {
        throw std::runtime_error("injected source failure");
    });
    auto recorded_failure = broken_stream.record_event();
    broken_gate.complete();
    require_throws<std::runtime_error>([&] { broken.wait(); }, "injected source did not fail");
    require_throws<std::runtime_error>([&] { recorded_failure.wait(); }, "recorded failure did not retire");
    // Accept both consumer operations before the already-failed dependency can retire them.
    Deferred consumer_gate;
    (void)consumer_gate.enqueue(consumer);
    consumer_gate.wait_registered();
    auto barrier = consumer.wait_event(recorded_failure);
    auto later = consumer.record_event();
    consumer_gate.complete();
    require_throws<std::runtime_error>([&] { barrier.wait(); }, "already-failed dependency barrier did not fail");
    require_throws<std::runtime_error>([&] { later.wait(); }, "consumer work after failed barrier did not fail");
    auto healthy = runtime.create_stream().record_event();
    healthy.wait();

    auto pending_source = runtime.create_stream();
    auto pending_consumer = runtime.create_stream();
    Deferred failing_gate;
    (void)failing_gate.enqueue(pending_source);
    failing_gate.wait_registered();
    auto pending_failure_event = pending_source.record_event();
    auto pending_barrier = pending_consumer.wait_event(pending_failure_event);
    auto skipped = pending_consumer.record_event();
    failing_gate.complete(std::make_exception_ptr(std::runtime_error("pending source failure")));
    require_throws<std::runtime_error>([&] { pending_barrier.wait(); }, "pending dependency failure was not forwarded");
    require_throws<std::runtime_error>([&] { skipped.wait(); }, "pending dependency did not fail later consumer operation");
}

void test_invalid_foreign_events_and_barrier_limit_rollback() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1, 2});
    auto stream = runtime.create_stream();
    gridforge::Event invalid;
    require_throws<std::invalid_argument>([&] { (void)stream.wait_event(invalid); }, "invalid Event should be rejected");
    gridforge::Runtime foreign_runtime;
    auto foreign = foreign_runtime.create_stream().record_event();
    foreign.wait();
    require_throws<std::invalid_argument>([&] { (void)stream.wait_event(foreign); }, "foreign Event should be rejected");

    Deferred gate;
    (void)gate.enqueue(stream);
    gate.wait_registered();
    auto marker = stream.record_event();
    // The deferred operation and event marker occupy the entire two-operation limit.
    require_throws<std::length_error>([&] { (void)stream.wait_event(marker); }, "operation limit did not reject dependency barrier");
    gate.complete();
    marker.wait();
    auto recovered = stream.wait_event(marker);
    recovered.wait();
}

void test_concurrent_registration_and_completion() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1});
    auto producer = runtime.create_stream();
    Deferred gate;
    (void)gate.enqueue(producer);
    gate.wait_registered();
    auto source_event = producer.record_event();
    constexpr std::size_t consumers = 12;
    std::vector<gridforge::Stream> streams;
    std::vector<gridforge::Event> barriers(consumers);
    streams.reserve(consumers);
    barriers.reserve(consumers);
    for (std::size_t i = 0; i < consumers; ++i) streams.push_back(runtime.create_stream());
    std::barrier start(static_cast<std::ptrdiff_t>(consumers + 1));
    std::vector<std::thread> registering;
    for (std::size_t index = 0; index < streams.size(); ++index) {
        registering.emplace_back([&, index] {
            start.arrive_and_wait();
            barriers[index] = streams[index].wait_event(source_event);
        });
    }
    start.arrive_and_wait();
    gate.complete();
    for (auto& thread : registering) thread.join();
    source_event.wait();
    for (const auto& barrier : barriers) barrier.wait();
}

void test_event_provenance_contract() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1, 8});
    auto source_stream = runtime.create_stream();
    auto consumer = runtime.create_stream();

    Deferred gate;
    (void)gate.enqueue(source_stream);
    gate.wait_registered();
    auto pending_recorded = source_stream.record_event();
    auto pending_barrier = consumer.wait_event(pending_recorded);
    require(!pending_recorded.is_complete(), "pending recorded source must remain incomplete");
    require_throws<std::invalid_argument>([&] { (void)consumer.wait_event(pending_barrier); },
        "pending barrier completion event must be rejected");
    gate.complete();
    pending_recorded.wait();
    pending_barrier.wait();

    auto completed_source = source_stream.record_event();
    completed_source.wait();
    auto completed_barrier = consumer.wait_event(completed_source);
    completed_barrier.wait();

    auto failing_stream = runtime.create_stream();
    auto failing_consumer = runtime.create_stream();
    Deferred failing_gate;
    (void)failing_gate.enqueue(failing_stream);
    failing_gate.wait_registered();
    auto failing_task = gridforge::detail::TestAccess::enqueue_test_task(failing_stream, [] {
        throw std::runtime_error("recorded dependency source failure");
    });
    auto failing_source = failing_stream.record_event();
    auto failing_barrier = failing_consumer.wait_event(failing_source);
    failing_gate.complete();
    require_throws<std::runtime_error>([&] { failing_task.wait(); },
        "recorded dependency source failure did not propagate");
    require_throws<std::runtime_error>([&] { failing_barrier.wait(); },
        "recorded dependency source failure did not propagate");

    auto ordinary_stream = runtime.create_stream();
    auto ordinary_event = gridforge::detail::TestAccess::enqueue_test_task(ordinary_stream, [] {});
    require_throws<std::invalid_argument>([&] { (void)consumer.wait_event(ordinary_event); },
        "ordinary task event must be rejected as dependency source");
    ordinary_event.wait();
    require_throws<std::invalid_argument>([&] { (void)consumer.wait_event(ordinary_event); },
        "completed ordinary task event must remain rejected");

    auto barrier_source = runtime.create_stream().record_event();
    auto barrier = consumer.wait_event(barrier_source);
    require_throws<std::invalid_argument>([&] { (void)consumer.wait_event(barrier); },
        "barrier completion event must be rejected while pending");
    barrier.wait();
    require_throws<std::invalid_argument>([&] { (void)consumer.wait_event(barrier); },
        "completed barrier completion event must remain rejected");

    gridforge::Event invalid;
    require_throws<std::invalid_argument>([&] { (void)consumer.wait_event(invalid); },
        "default event must be rejected");
    gridforge::Runtime foreign_runtime;
    auto foreign = foreign_runtime.create_stream().record_event();
    foreign.wait();
    require_throws<std::invalid_argument>([&] { (void)consumer.wait_event(foreign); },
        "foreign runtime event must be rejected");

    auto same_stream = runtime.create_stream();
    auto same_stream_source = same_stream.record_event();
    auto same_stream_wait = same_stream.wait_event(same_stream_source);
    same_stream_wait.wait();

    // Rejected waits must not consume queue slots, subscriptions, or reservation accounting.
    auto verified_stream = runtime.create_stream();
    auto verified_source = verified_stream.record_event();
    auto task_event = gridforge::detail::TestAccess::enqueue_test_task(verified_stream, [] {});
    require_throws<std::invalid_argument>([&] { (void)verified_stream.wait_event(task_event); },
        "rejected ordinary event must not consume a dependency slot");
    auto valid_after_rejection = verified_stream.wait_event(verified_source);
    valid_after_rejection.wait();
}

void test_dependency_shutdown_and_event_lifetimes() {
    auto runtime = std::make_unique<gridforge::Runtime>(gridforge::RuntimeOptions{
        gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 1});
    auto producer = runtime->create_stream();
    auto consumer = runtime->create_stream();
    Deferred gate;
    (void)gate.enqueue(producer);
    gate.wait_registered();
    auto source = producer.record_event();
    auto barrier = consumer.wait_event(source);
    source = gridforge::Event{}; // accepted barrier retains event state
    producer = gridforge::Stream{};
    consumer = gridforge::Stream{};
    std::latch started(1);
    std::thread destroyer([owned = std::move(runtime), &started]() mutable {
        started.count_down();
        owned.reset();
    });
    started.wait();
    gate.complete();
    destroyer.join();
    barrier.wait();
    require(barrier.is_complete(), "barrier event should remain inspectable after runtime shutdown");
}

void test_deferred_completion_retirement_stress() {
    gridforge::Runtime runtime({gridforge::Backend::CPU, gridforge::default_max_allocated_bytes, 4});
    std::array<gridforge::Stream, 4> streams{
        runtime.create_stream(), runtime.create_stream(), runtime.create_stream(), runtime.create_stream()};
    // Repeatedly race callback publication against the retirement thread entering its wait.
    // Do not submit unrelated work that could accidentally wake a stranded completion.
    for (std::size_t iteration = 0; iteration < 1000; ++iteration) {
        Deferred completion;
        auto event = completion.enqueue(streams[iteration % streams.size()]);
        completion.wait_registered();
        completion.complete();
        event.wait();
        require(event.is_complete(), "deferred completion failed to retire");
    }
}

void run_all() {
    test_three_stream_transitive_handoff_single_worker();
    test_completed_and_self_dependencies_and_multiple_barriers();
    test_blocked_consumer_does_not_occupy_only_worker();
    test_prefix_coverage_and_unordered_conflicts();
    test_read_read_sharing_and_failed_dependency();
    test_invalid_foreign_events_and_barrier_limit_rollback();
    test_event_provenance_contract();
    test_concurrent_registration_and_completion();
    test_dependency_shutdown_and_event_lifetimes();
    test_deferred_completion_retirement_stress();
}

} // namespace

int main() {
    try {
        run_all();
        std::cout << "PASS: cross-stream dependency tests\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
