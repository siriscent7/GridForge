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

bool verify(const gridforge::DownloadResult& result, const std::vector<float>& expected) {
    const auto bytes = result.get();
    if (bytes.size() != expected.size() * sizeof(float)) return false;
    std::vector<float> actual(expected.size());
    if (!actual.empty()) std::memcpy(actual.data(), bytes.data(), bytes.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (!close_enough(actual[i], expected[i])) return false;
    }
    return true;
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
            throw std::invalid_argument("Usage: gridforge_async_demo [--backend cpu|metal]");
        }
        constexpr std::size_t elements_per_stream = 250'000;
        gridforge::Runtime runtime({backend});
        auto stream_a = runtime.create_stream();
        auto stream_b = runtime.create_stream();

        std::mt19937 generator(0x47524632U);
        std::uniform_real_distribution<float> distribution(-500.0F, 500.0F);
        std::vector<float> a0(elements_per_stream), b0(elements_per_stream);
        std::vector<float> a1(elements_per_stream), b1(elements_per_stream);
        std::vector<float> expected0(elements_per_stream), expected1(elements_per_stream);
        for (std::size_t i = 0; i < elements_per_stream; ++i) {
            a0[i] = distribution(generator);
            b0[i] = distribution(generator);
            a1[i] = distribution(generator);
            b1[i] = distribution(generator);
            expected0[i] = a0[i] + b0[i];
            expected1[i] = a1[i] + b1[i];
        }

        const auto bytes = elements_per_stream * sizeof(float);
        auto a_buffer0 = runtime.create_buffer(bytes);
        auto b_buffer0 = runtime.create_buffer(bytes);
        auto c_buffer0 = runtime.create_buffer(bytes);
        auto a_buffer1 = runtime.create_buffer(bytes);
        auto b_buffer1 = runtime.create_buffer(bytes);
        auto c_buffer1 = runtime.create_buffer(bytes);

        stream_a.upload_async(a_buffer0, std::as_bytes(std::span<const float>(a0)));
        stream_a.upload_async(b_buffer0, std::as_bytes(std::span<const float>(b0)));
        stream_a.vector_add_async(a_buffer0, b_buffer0, c_buffer0, elements_per_stream);
        auto result0 = stream_a.download_async(c_buffer0);

        stream_b.upload_async(a_buffer1, std::as_bytes(std::span<const float>(a1)));
        stream_b.upload_async(b_buffer1, std::as_bytes(std::span<const float>(b1)));
        stream_b.vector_add_async(a_buffer1, b_buffer1, c_buffer1, elements_per_stream);
        auto result1 = stream_b.download_async(c_buffer1);

        const bool verified = verify(result0, expected0) && verify(result1, expected1);
        std::cout << "Backend: " << runtime.backend_name() << '\n'
                  << "Device: " << runtime.device_name() << '\n'
                  << "Configured host workers: " << runtime.worker_count()
                  << " (not the GPU thread count)\n"
                  << "Streams: 2\n"
                  << "Elements per stream: " << elements_per_stream << '\n'
                  << "Verification: " << (verified ? "PASS" : "FAIL") << '\n';
        return verified ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << "GridForge async demo error: " << error.what() << '\n';
        return 1;
    }
}
