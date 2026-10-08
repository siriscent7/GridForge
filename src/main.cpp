#include "gridforge/gridforge.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

struct Arguments {
    gridforge::Backend backend{gridforge::Backend::CPU};
    std::size_t elements{1'000'000};
};

Arguments parse_arguments(int argc, char** argv) {
    Arguments result;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--backend" && i + 1 < argc) {
            const std::string_view value(argv[++i]);
            if (value == "cpu") result.backend = gridforge::Backend::CPU;
            else if (value == "metal") result.backend = gridforge::Backend::Metal;
            else throw std::invalid_argument("--backend must be cpu or metal.");
        } else if (argument == "--elements" && i + 1 < argc) {
            const std::string_view value(argv[++i]);
            std::size_t parsed = 0;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (error != std::errc{} || end != value.data() + value.size()) {
                throw std::invalid_argument("--elements must be a non-negative integer.");
            }
            result.elements = parsed;
        } else {
            throw std::invalid_argument("Usage: gridforge_demo [--backend cpu|metal] [--elements COUNT]");
        }
    }
    return result;
}

bool close_enough(float actual, float expected) {
    constexpr float absolute_tolerance = 1.0e-5F;
    constexpr float relative_tolerance = 1.0e-5F;
    return std::isfinite(actual) && std::isfinite(expected) &&
           std::abs(actual - expected) <= absolute_tolerance + relative_tolerance * std::abs(expected);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Arguments arguments = parse_arguments(argc, argv);
        if (arguments.elements > gridforge::default_max_allocated_bytes / (3 * sizeof(float))) {
            throw std::length_error("Requested inputs and output exceed the default combined 512 MiB buffer limit.");
        }

        const auto start = std::chrono::steady_clock::now();
        gridforge::Runtime runtime({arguments.backend});
        std::vector<float> a(arguments.elements);
        std::vector<float> b(arguments.elements);
        std::vector<float> expected(arguments.elements);
        std::vector<float> result(arguments.elements);
        std::mt19937 generator(0x47524944U);
        std::uniform_real_distribution<float> distribution(-1000.0F, 1000.0F);
        for (std::size_t i = 0; i < arguments.elements; ++i) {
            a[i] = distribution(generator);
            b[i] = distribution(generator);
            expected[i] = a[i] + b[i];
        }

        const std::size_t bytes = arguments.elements * sizeof(float);
        auto buffer_a = runtime.create_buffer(bytes);
        auto buffer_b = runtime.create_buffer(bytes);
        auto buffer_c = runtime.create_buffer(bytes);
        buffer_a.upload(std::as_bytes(std::span<const float>(a)));
        buffer_b.upload(std::as_bytes(std::span<const float>(b)));
        runtime.vector_add(buffer_a, buffer_b, buffer_c, arguments.elements);
        runtime.synchronize();
        buffer_c.download(std::as_writable_bytes(std::span<float>(result)));

        bool verified = true;
        std::size_t mismatch = 0;
        for (std::size_t i = 0; i < arguments.elements; ++i) {
            if (!close_enough(result[i], expected[i])) {
                verified = false;
                mismatch = i;
                break;
            }
        }
        const auto end = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double, std::milli>(end - start).count();

        std::cout << "Backend: " << runtime.backend_name() << '\n'
                  << "Device: " << runtime.device_name() << '\n'
                  << "Elements: " << arguments.elements << '\n'
                  << "Verification: " << (verified ? "PASS" : "FAIL") << '\n'
                  << "Demonstration measurement — end-to-end wall-clock duration: " << elapsed << " ms\n";
        if (!verified) {
            std::cerr << "First mismatch at index " << mismatch << ": expected " << expected[mismatch]
                      << ", received " << result[mismatch] << '\n';
            return 2;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GridForge demo error: " << error.what() << '\n';
        return 1;
    }
}