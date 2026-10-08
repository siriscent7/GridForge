#include "gridforge/gridforge.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <random>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

bool near(float value, float expected) {
    constexpr float abs_tol = 1.0e-5F;
    constexpr float rel_tol = 1.0e-5F;
    return std::isfinite(value) && std::isfinite(expected) &&
        std::abs(value - expected) <= abs_tol + rel_tol * std::abs(expected);
}

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

} // namespace

int main() {
    try {
        try {
            gridforge::Runtime available({gridforge::Backend::Metal});
            (void)available.device_name();
        } catch (const gridforge::BackendUnavailable& error) {
            std::cout << "SKIP: " << error.what() << '\n';
            return 77;
        }

        constexpr std::size_t count = 4097;
        gridforge::Runtime runtime({gridforge::Backend::Metal, gridforge::default_max_allocated_bytes, 1});
        auto producer = runtime.create_stream();
        auto intermediate = runtime.create_stream();
        auto consumer = runtime.create_stream();
        std::vector<float> a(count), b(count), expected(count);
        std::mt19937 generator(0xD34U);
        std::uniform_real_distribution<float> distribution(-100.0F, 100.0F);
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
        auto output = consumer.download_async(bd);
        auto final_event = consumer.record_event();
        final_event.wait();

        const auto bytes = output.get();
        require(bytes.size() == expected.size() * sizeof(float), "dependency Metal output size mismatch");
        for (std::size_t i = 0; i < count; ++i) {
            float actual;
            std::memcpy(&actual, bytes.data() + i * sizeof(float), sizeof(float));
            require(near(actual, expected[i]), "dependency Metal output mismatch");
        }
        std::cout << "PASS: hardware-backed Metal event dependencies\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
