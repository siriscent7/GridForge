#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace gridforge::bench_support {

struct MeasurementSlot {
    std::size_t case_index;
    std::size_t backend_index;
};

inline MeasurementSlot measurement_slot(std::size_t round, std::size_t case_offset,
                                        std::size_t case_count, std::size_t backend_offset,
                                        std::size_t backend_count) {
    if (case_count == 0 || backend_count == 0 || case_offset >= case_count || backend_offset >= backend_count)
        throw std::invalid_argument("Invalid measurement schedule coordinates.");
    const std::size_t case_index = (round % case_count + case_offset) % case_count;
    // Use the logical case index. The rotating offset would cancel round parity.
    return {case_index, (round + case_index + backend_offset) % backend_count};
}

struct Statistics {
    std::size_t sample_count{0U};
    double min{0.0};
    double median{0.0};
    double mean{0.0};
    double sample_stddev{0.0};
    double max{0.0};
    double p95{0.0};
};

inline Statistics compute_statistics(const std::vector<double>& samples) {
    Statistics stats{};
    if (samples.empty()) return stats;
    stats.sample_count = samples.size();
    stats.min = *std::min_element(samples.begin(), samples.end());
    stats.max = *std::max_element(samples.begin(), samples.end());
    double sum = 0.0;
    for (double sample : samples) sum += sample;
    stats.mean = sum / static_cast<double>(samples.size());

    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t middle = sorted.size() / 2U;
    stats.median = sorted.size() % 2U == 0U
        ? 0.5 * (sorted[middle - 1U] + sorted[middle])
        : sorted[middle];

    double squared_deviations = 0.0;
    for (double sample : samples) {
        const double delta = sample - stats.mean;
        squared_deviations += delta * delta;
    }
    stats.sample_stddev = std::sqrt(squared_deviations / static_cast<double>(std::max<std::size_t>(1U, samples.size() - 1U)));
    const std::size_t p95_index = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(sorted.size()))) - 1U;
    stats.p95 = sorted[p95_index];
    return stats;
}

inline std::vector<std::size_t> partition_elements(std::size_t total, std::size_t partition_count) {
    if (partition_count == 0U || partition_count > total) {
        throw std::invalid_argument("Partition count must be between one and the total element count.");
    }
    std::vector<std::size_t> partitions(partition_count, total / partition_count);
    for (std::size_t index = 0; index < total % partition_count; ++index) {
        ++partitions[index];
    }
    return partitions;
}

} // namespace gridforge::bench_support
