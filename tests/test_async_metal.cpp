#include "gridforge/gridforge.hpp"
#include "test_hooks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <latch>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool close_enough(float actual, float expected) {
    constexpr float absolute_tolerance = 1.0e-5F;
    constexpr float relative_tolerance = 1.0e-5F;
    return std::isfinite(actual) && std::isfinite(expected) &&
           std::abs(actual - expected) <= absolute_tolerance + relative_tolerance * std::abs(expected);
}

std::vector<float> download_floats(const gridforge::DownloadResult& result) {
    const auto bytes = result.get();
    require(bytes.size() % sizeof(float) == 0, "Metal result byte length is not float-aligned");
    std::vector<float> values(bytes.size() / sizeof(float));
    if (!bytes.empty()) std::memcpy(values.data(), bytes.data(), bytes.size());
    return values;
}

void run_case(gridforge::Runtime& runtime, gridforge::Stream& stream,
              std::size_t count, std::uint32_t seed) {
    std::mt19937 generator(seed);
    std::uniform_real_distribution<float> distribution(-1000.0F, 1000.0F);
    std::vector<float> a(count), b(count), expected(count);
    for (std::size_t i = 0; i < count; ++i) {
        a[i] = distribution(generator);
        b[i] = distribution(generator);
        expected[i] = a[i] + b[i];
    }
    auto ba = runtime.create_buffer(count * sizeof(float));
    auto bb = runtime.create_buffer(count * sizeof(float));
    auto bc = runtime.create_buffer(count * sizeof(float));
    stream.upload_async(ba, std::as_bytes(std::span<const float>(a)));
    stream.upload_async(bb, std::as_bytes(std::span<const float>(b)));
    std::fill(a.begin(), a.end(), 123456.0F);
    std::fill(b.begin(), b.end(), -123456.0F);
    stream.vector_add_async(ba, bb, bc, count);
    auto result = stream.download_async(bc);
    auto event = stream.record_event();
    event.wait();
    const auto actual = download_floats(result);
    for (std::size_t i = 0; i < count; ++i) {
        require(close_enough(actual[i], expected[i]), "Metal async result mismatch at index " + std::to_string(i));
    }
}

void test_multistream_and_reuse() {
    gridforge::Runtime runtime({gridforge::Backend::Metal, gridforge::default_max_allocated_bytes, 3});
    require(runtime.backend() == gridforge::Backend::Metal, "explicit Metal runtime silently changed backend");
    auto stream_a = runtime.create_stream();
    auto stream_b = runtime.create_stream();
    for (const auto count : {0U, 1U, 255U, 256U, 257U, 19U, 513U, 4097U}) {
        run_case(runtime, stream_a, count, static_cast<std::uint32_t>(0x7700U + count));
    }
    run_case(runtime, stream_b, 4097, 0x9911U);
    for (int repetition = 0; repetition < 4; ++repetition) {
        const std::size_t count = 255 + static_cast<std::size_t>(repetition);
        run_case(runtime, stream_a, count, static_cast<std::uint32_t>(repetition + 1));
    }
    constexpr std::size_t many_streams = 12;
    std::vector<gridforge::Stream> streams;
    std::vector<gridforge::DownloadResult> results;
    std::vector<std::vector<float>> expected(many_streams);
    std::vector<gridforge::Buffer> left, right, output;
    streams.reserve(many_streams);
    results.reserve(many_streams);
    left.reserve(many_streams);
    right.reserve(many_streams);
    output.reserve(many_streams);
    for (std::size_t stream_index = 0; stream_index < many_streams; ++stream_index) {
        streams.push_back(runtime.create_stream());
        const auto count = stream_index + 3;
        std::mt19937 random(static_cast<std::uint32_t>(0xA00U + stream_index));
        std::uniform_real_distribution<float> values(-10.0F, 10.0F);
        std::vector<float> a(count), b(count);
        expected[stream_index].resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            a[i] = values(random);
            b[i] = values(random);
            expected[stream_index][i] = a[i] + b[i];
        }
        left.push_back(runtime.create_buffer(count * sizeof(float)));
        right.push_back(runtime.create_buffer(count * sizeof(float)));
        output.push_back(runtime.create_buffer(count * sizeof(float)));
        streams.back().upload_async(left.back(), std::as_bytes(std::span<const float>(a)));
        streams.back().upload_async(right.back(), std::as_bytes(std::span<const float>(b)));
        streams.back().vector_add_async(left.back(), right.back(), output.back(), count);
        results.push_back(streams.back().download_async(output.back()));
    }
    for (std::size_t stream_index = 0; stream_index < many_streams; ++stream_index) {
        const auto actual = download_floats(results[stream_index]);
        require(actual.size() == expected[stream_index].size(), "multi-stream result size mismatch");
        for (std::size_t i = 0; i < actual.size(); ++i) {
            require(close_enough(actual[i], expected[stream_index][i]), "multi-stream Metal result mismatch");
        }
    }
    runtime.synchronize();
}

void test_pending_public_handle_destruction_and_post_runtime_result() {
    std::unique_ptr<gridforge::DownloadResult> retained;
    std::unique_ptr<gridforge::Event> completion;
    {
        auto runtime = std::make_unique<gridforge::Runtime>(gridforge::RuntimeOptions{
            gridforge::Backend::Metal, gridforge::default_max_allocated_bytes, 1});
        auto stream = runtime->create_stream();
        std::latch worker_entered(1);
        std::latch release_worker(1);
        auto worker_gate = gridforge::detail::TestAccess::enqueue_test_task(stream, [&] {
            worker_entered.count_down();
            release_worker.wait();
        });
        worker_entered.wait();
        auto ba = runtime->create_buffer(3 * sizeof(float));
        auto bb = runtime->create_buffer(3 * sizeof(float));
        auto bc = runtime->create_buffer(3 * sizeof(float));
        {
            std::vector<float> a{3.0F, -2.0F, 100.0F};
            std::vector<float> b{-1.0F, 8.0F, -50.0F};
            stream.upload_async(ba, std::as_bytes(std::span<const float>(a)));
            stream.upload_async(bb, std::as_bytes(std::span<const float>(b)));
        } // Both caller-owned source vectors are destroyed before the uploads are released.
        stream.vector_add_async(ba, bb, bc, 3);
        retained = std::make_unique<gridforge::DownloadResult>(stream.download_async(bc));
        completion = std::make_unique<gridforge::Event>(stream.record_event());
        ba = gridforge::Buffer{};
        bb = gridforge::Buffer{};
        bc = gridforge::Buffer{};
        stream = gridforge::Stream{};
        release_worker.count_down();
        worker_gate.wait();
        runtime.reset(); // must drain GPU completion and retirement before returning
    }
    completion->wait();
    const auto values = download_floats(*retained);
    require(values == std::vector<float>({2.0F, 6.0F, 50.0F}), "result/event did not survive runtime and public-handle destruction");
}

void test_metal_retained_download_result_limit() {
    gridforge::Runtime runtime({gridforge::Backend::Metal,
        gridforge::default_max_allocated_bytes, 2,
        gridforge::default_max_outstanding_operations, 16});
    auto stream = runtime.create_stream();
    auto buffer = runtime.create_buffer(4 * sizeof(float));
    const std::array<float, 4> values{1.0F, -2.0F, 3.5F, 8.0F};
    stream.upload_async(buffer, std::as_bytes(std::span(values)));
    stream.synchronize();
    auto retained = stream.download_async(buffer);
    retained.wait();
    bool rejected = false;
    try {
        (void)stream.download_async(buffer, 0, 1);
    } catch (const std::length_error&) {
        rejected = true;
    }
    require(rejected, "completed Metal download must continue consuming staging budget");
    retained = gridforge::DownloadResult{};
    auto after_release = stream.download_async(buffer, 0, 1);
    after_release.wait();
}

void test_metal_shared_buffer_access_conflicts() {
    gridforge::Runtime runtime({gridforge::Backend::Metal,
        gridforge::default_max_allocated_bytes, 1});
    auto first = runtime.create_stream();
    auto second = runtime.create_stream();
    auto shared = runtime.create_buffer(sizeof(float));
    std::latch entered(1);
    std::latch release(1);
    auto gate = gridforge::detail::TestAccess::enqueue_test_task(first, [&] {
        entered.count_down();
        release.wait();
    });
    entered.wait();
    const float value = -7.0F;
    first.upload_async(shared, std::as_bytes(std::span(&value, 1)));
    bool cross_stream_rejected = false;
    try {
        second.upload_async(shared, std::as_bytes(std::span(&value, 1)));
    } catch (const std::logic_error&) {
        cross_stream_rejected = true;
    }
    require(cross_stream_rejected, "Metal cross-stream conflicting write was accepted");
    bool synchronous_rejected = false;
    try {
        shared.upload(std::as_bytes(std::span(&value, 1)));
    } catch (const std::logic_error&) {
        synchronous_rejected = true;
    }
    require(synchronous_rejected, "Metal synchronous write conflicted with pending async access");
    release.count_down();
    gate.wait();
    first.synchronize();
}

void test_metal_transitive_event_handoff() {
    gridforge::Runtime runtime({gridforge::Backend::Metal});
    auto producer = runtime.create_stream();
    auto intermediate = runtime.create_stream();
    auto consumer = runtime.create_stream();
    constexpr std::size_t count = 4097;
    std::mt19937 generator(0x4D334U);
    std::uniform_real_distribution<float> distribution(-300.0F, 300.0F);
    std::vector<float> a(count), b(count), expected(count);
    for (std::size_t i = 0; i < count; ++i) {
        a[i] = distribution(generator);
        b[i] = distribution(generator);
        expected[i] = a[i] + 2.0F * b[i];
    }
    auto ba = runtime.create_buffer(count * sizeof(float));
    auto bb = runtime.create_buffer(count * sizeof(float));
    auto bc = runtime.create_buffer(count * sizeof(float));
    auto bd = runtime.create_buffer(count * sizeof(float));

    producer.upload_async(ba, std::as_bytes(std::span<const float>(a)));
    producer.upload_async(bb, std::as_bytes(std::span<const float>(b)));
    producer.vector_add_async(ba, bb, bc, count);
    auto e1 = producer.record_event();
    (void)intermediate.wait_event(e1);
    intermediate.vector_add_async(bc, bb, bd, count);
    auto e2 = intermediate.record_event();
    (void)consumer.wait_event(e2);
    auto result = consumer.download_async(bd);
    auto final_event = consumer.record_event();
    final_event.wait(); // One final wait observes the complete chain and populated result.
    const auto actual = download_floats(result);
    for (std::size_t i = 0; i < count; ++i) {
        require(close_enough(actual[i], expected[i]), "transitive Metal event handoff mismatch");
    }
}

void run_hardware_suite() {
    test_multistream_and_reuse();
    test_pending_public_handle_destruction_and_post_runtime_result();
    test_metal_retained_download_result_limit();
    test_metal_shared_buffer_access_conflicts();
    test_metal_transitive_event_handoff();
}

} // namespace

int main() {
    try {
        try {
            gridforge::Runtime availability({gridforge::Backend::Metal});
            (void)availability.device_name();
        } catch (const gridforge::BackendUnavailable& error) {
            std::cout << "SKIP: " << error.what() << '\n';
            return 77;
        }
        run_hardware_suite();
        std::cout << "PASS: asynchronous Metal stream tests\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
