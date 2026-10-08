#include "gridforge/gridforge.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

void run_case(gridforge::Runtime& runtime, std::size_t count, std::uint32_t seed) {
    std::mt19937 generator(seed);
    std::uniform_real_distribution<float> distribution(-1000.0F, 1000.0F);
    std::vector<float> a(count);
    std::vector<float> b(count);
    std::vector<float> expected(count);
    std::vector<float> output(count, -12345.0F);
    for (std::size_t i = 0; i < count; ++i) {
        a[i] = distribution(generator);
        b[i] = distribution(generator);
        expected[i] = a[i] + b[i];
    }

    const std::size_t bytes = count * sizeof(float);
    auto buffer_a = runtime.create_buffer(bytes);
    auto buffer_b = runtime.create_buffer(bytes);
    auto buffer_c = runtime.create_buffer(bytes);
    buffer_a.upload(std::as_bytes(std::span<const float>(a)));
    buffer_b.upload(std::as_bytes(std::span<const float>(b)));
    runtime.vector_add(buffer_a, buffer_b, buffer_c, count);
    runtime.synchronize();
    buffer_c.download(std::as_writable_bytes(std::span<float>(output)));
    for (std::size_t i = 0; i < count; ++i) {
        require(close_enough(output[i], expected[i]), "vector_add mismatch at index " + std::to_string(i));
    }
}

void test_independent_expected_values(gridforge::Runtime& runtime) {
    const std::vector<float> a{-3.0F, 0.0F, 1.25F, -100.0F};
    const std::vector<float> b{2.0F, -5.0F, 8.0F, 99.5F};
    const std::vector<float> expected{-1.0F, -5.0F, 9.25F, -0.5F};
    auto buffer_a = runtime.create_buffer(a.size() * sizeof(float));
    auto buffer_b = runtime.create_buffer(b.size() * sizeof(float));
    auto buffer_c = runtime.create_buffer(expected.size() * sizeof(float));
    buffer_a.upload(std::as_bytes(std::span<const float>(a)));
    buffer_b.upload(std::as_bytes(std::span<const float>(b)));
    runtime.vector_add(buffer_a, buffer_b, buffer_c, expected.size());
    std::vector<float> output(expected.size());
    buffer_c.download(std::as_writable_bytes(std::span<float>(output)));
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(close_enough(output[i], expected[i]), "independent expected-value case failed");
    }
}

void test_repeated_launch_and_reuse(gridforge::Runtime& runtime) {
    constexpr std::size_t count = 257;
    std::vector<float> a(count);
    std::vector<float> b(count);
    std::vector<float> output(count);
    auto buffer_a = runtime.create_buffer(count * sizeof(float));
    auto buffer_b = runtime.create_buffer(count * sizeof(float));
    auto buffer_c = runtime.create_buffer(count * sizeof(float));

    for (int launch = 0; launch < 4; ++launch) {
        for (std::size_t i = 0; i < count; ++i) {
            a[i] = static_cast<float>(static_cast<int>(i % 31) - 15) * static_cast<float>(launch + 1);
            b[i] = -static_cast<float>(i % 17) - static_cast<float>(launch);
        }
        buffer_a.upload(std::as_bytes(std::span<const float>(a)));
        buffer_b.upload(std::as_bytes(std::span<const float>(b)));
        runtime.vector_add(buffer_a, buffer_b, buffer_c, count);
        buffer_c.download(std::as_writable_bytes(std::span<float>(output)));
        for (std::size_t i = 0; i < count; ++i) {
            require(close_enough(output[i], a[i] + b[i]), "reused buffers produced an incorrect result");
        }
    }
}

void test_invalid_operations(gridforge::Runtime& runtime) {
    auto short_a = runtime.create_buffer(sizeof(float));
    auto long_b = runtime.create_buffer(2 * sizeof(float));
    auto short_c = runtime.create_buffer(sizeof(float));
    require_throws<std::invalid_argument>([&] { runtime.vector_add(short_a, long_b, short_c, 1); },
                                          "mismatched buffer sizes must be rejected");
    require_throws<std::invalid_argument>([&] { runtime.vector_add(short_a, short_a, short_c, 1); },
                                          "aliased input buffers must be rejected");
    require_throws<std::out_of_range>([&] {
        const std::array<std::byte, 2> source{};
        short_a.upload(source, sizeof(float));
    }, "out-of-range upload must be rejected");
    require_throws<std::out_of_range>([&] {
        std::array<std::byte, 2> destination{};
        short_a.download(destination, sizeof(float));
    }, "out-of-range download must be rejected");

    gridforge::Runtime other_runtime;
    auto foreign = other_runtime.create_buffer(sizeof(float));
    require_throws<std::invalid_argument>([&] { runtime.vector_add(short_a, short_c, foreign, 1); },
                                          "buffers from another runtime must be rejected");
}

void test_allocation_limit() {
    gridforge::Runtime limited({gridforge::Backend::CPU, 16});
    auto first = limited.create_buffer(12);
    require_throws<std::length_error>([&] { (void)limited.create_buffer(5); },
                                      "combined allocation limit must be enforced");
    first = gridforge::Buffer{};
    auto after_release = limited.create_buffer(16);
    require(after_release.size_bytes() == 16, "buffer destruction must release allocation budget");
}

void test_explicit_metal_selection() {
    try {
        gridforge::Runtime metal({gridforge::Backend::Metal});
        require(metal.backend() == gridforge::Backend::Metal, "Metal selection must not fall back to CPU");
        require(std::string_view(metal.device_name()).size() > 0, "Metal device name must be reported");
        auto stream = metal.create_stream();
        require(!stream.is_closed(), "Metal async stream should be open");
    } catch (const gridforge::BackendUnavailable& error) {
        std::cout << "Metal unavailable as expected for this build/device: " << error.what() << '\n';
    }
}

void test_cpu_metal_agreement() {
    try {
        gridforge::Runtime cpu;
        gridforge::Runtime metal({gridforge::Backend::Metal});
        for (const std::size_t count : {0U, 1U, 255U, 256U, 257U, 2U, 17U, 513U, 1023U, 4097U}) {
            std::mt19937 generator(static_cast<std::uint32_t>(0x9876U + count));
            std::uniform_real_distribution<float> distribution(-1000.0F, 1000.0F);
            std::vector<float> a(count);
            std::vector<float> b(count);
            std::vector<float> cpu_output(count);
            std::vector<float> metal_output(count);
            for (std::size_t i = 0; i < count; ++i) {
                a[i] = distribution(generator);
                b[i] = distribution(generator);
            }

            const std::size_t bytes = count * sizeof(float);
            auto cpu_a = cpu.create_buffer(bytes);
            auto cpu_b = cpu.create_buffer(bytes);
            auto cpu_c = cpu.create_buffer(bytes);
            auto metal_a = metal.create_buffer(bytes);
            auto metal_b = metal.create_buffer(bytes);
            auto metal_c = metal.create_buffer(bytes);
            cpu_a.upload(std::as_bytes(std::span<const float>(a)));
            cpu_b.upload(std::as_bytes(std::span<const float>(b)));
            metal_a.upload(std::as_bytes(std::span<const float>(a)));
            metal_b.upload(std::as_bytes(std::span<const float>(b)));
            cpu.vector_add(cpu_a, cpu_b, cpu_c, count);
            metal.vector_add(metal_a, metal_b, metal_c, count);
            cpu_c.download(std::as_writable_bytes(std::span<float>(cpu_output)));
            metal_c.download(std::as_writable_bytes(std::span<float>(metal_output)));
            for (std::size_t i = 0; i < count; ++i) {
                require(close_enough(cpu_output[i], metal_output[i]), "CPU and Metal results differ");
            }
        }
    } catch (const gridforge::BackendUnavailable& error) {
        std::cout << "CPU/Metal comparison skipped because Metal is unavailable: " << error.what() << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3 || std::string_view(argv[1]) != "--backend") {
        std::cerr << "Usage: gridforge_tests --backend cpu|metal\n";
        return 2;
    }
    const std::string_view backend_arg(argv[2]);
    gridforge::Backend backend;
    if (backend_arg == "cpu") backend = gridforge::Backend::CPU;
    else if (backend_arg == "metal") backend = gridforge::Backend::Metal;
    else {
        std::cerr << "Unknown test backend.\n";
        return 2;
    }

    try {
        std::unique_ptr<gridforge::Runtime> runtime;
        try {
            runtime = std::make_unique<gridforge::Runtime>(gridforge::RuntimeOptions{backend});
        } catch (const gridforge::BackendUnavailable& error) {
            if (backend == gridforge::Backend::Metal) {
                std::cout << "SKIP: " << error.what() << '\n';
                return 77;
            }
            throw;
        }
        require(runtime->backend() == backend, "requested backend was not selected");
        if (backend == gridforge::Backend::Metal) {
            std::cout << "Metal test device: " << runtime->device_name() << '\n';
        }

        test_independent_expected_values(*runtime);
        for (const std::size_t count : {0U, 1U, 255U, 256U, 257U, 2U, 17U, 513U, 1023U, 4097U}) {
            run_case(*runtime, count, static_cast<std::uint32_t>(0x1234U + count));
        }
        test_repeated_launch_and_reuse(*runtime);
        test_invalid_operations(*runtime);
        test_allocation_limit();
        if (backend == gridforge::Backend::CPU) {
            test_explicit_metal_selection();
            test_cpu_metal_agreement();
        }

        std::cout << "PASS: " << runtime->backend_name() << " runtime tests\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}