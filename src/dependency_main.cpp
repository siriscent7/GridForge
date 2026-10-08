#include "gridforge/gridforge.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

bool close_enough(float actual, float expected) {
    constexpr float absolute_tolerance = 1.0e-5F;
    constexpr float relative_tolerance = 1.0e-5F;
    return std::isfinite(actual) && std::isfinite(expected) &&
        std::abs(actual - expected) <= absolute_tolerance + relative_tolerance * std::abs(expected);
}

} // namespace

int main(int argc, char** argv) {
    try {
        gridforge::Backend backend = gridforge::Backend::CPU;
        if (argc == 3 && std::string_view(argv[1]) == "--backend") {
            const std::string_view value(argv[2]);
            if (value == "cpu") backend = gridforge::Backend::CPU;
            else if (value == "metal") backend = gridforge::Backend::Metal;
            else throw std::invalid_argument("--backend must be cpu or metal.");
        } else if (argc != 1) {
            throw std::invalid_argument("Usage: gridforge_dependency_demo [--backend cpu|metal]");
        }

        constexpr std::size_t count = 250'000;
        gridforge::Runtime runtime({backend});
        auto producer = runtime.create_stream();
        auto intermediate = runtime.create_stream();
        auto consumer = runtime.create_stream();
        auto a_buffer = runtime.create_buffer(count * sizeof(float));
        auto b_buffer = runtime.create_buffer(count * sizeof(float));
        auto c_buffer = runtime.create_buffer(count * sizeof(float));
        auto d_buffer = runtime.create_buffer(count * sizeof(float));

        std::mt19937 generator(0x4D344752U);
        std::uniform_real_distribution<float> distribution(-250.0F, 250.0F);
        std::vector<float> a(count), b(count), expected(count);
        for (std::size_t i = 0; i < count; ++i) {
            a[i] = distribution(generator);
            b[i] = distribution(generator);
            expected[i] = a[i] + 2.0F * b[i];
        }

        // Producer: A + B -> C, then mark the completed prefix.
        producer.upload_async(a_buffer, std::as_bytes(std::span<const float>(a)));
        producer.upload_async(b_buffer, std::as_bytes(std::span<const float>(b)));
        producer.vector_add_async(a_buffer, b_buffer, c_buffer, count);
        auto e1 = producer.record_event();

        // Intermediate: depend on E1, compute C + B -> D, then mark E2.
        (void)intermediate.wait_event(e1);
        intermediate.vector_add_async(c_buffer, b_buffer, d_buffer, count);
        auto e2 = intermediate.record_event();

        // Consumer: depend on E2 and retrieve the owned output. No producer/intermediate
        // synchronization is performed on the calling thread before this submission.
        (void)consumer.wait_event(e2);
        auto result = consumer.download_async(d_buffer);
        const auto output_bytes = result.get(); // The sole final wait for the dependency chain.
        bool verified = output_bytes.size() == expected.size() * sizeof(float);
        for (std::size_t i = 0; verified && i < count; ++i) {
            float value;
            std::memcpy(&value, output_bytes.data() + i * sizeof(float), sizeof(float));
            verified = close_enough(value, expected[i]);
        }

        std::cout << "Backend: " << runtime.backend_name() << '\n'
                  << "Device: " << runtime.device_name() << '\n'
                  << "Streams: 3\n"
                  << "Elements: " << count << '\n'
                  << "Verification: " << (verified ? "PASS" : "FAIL") << '\n';
        return verified ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "GridForge dependency demo error: " << error.what() << '\n';
        return 1;
    }
}
