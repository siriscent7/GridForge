#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace gridforge::detail {

class MetalBuffer {
public:
    virtual ~MetalBuffer() = default;
    [[nodiscard]] virtual std::size_t size_bytes() const noexcept = 0;
};

class MetalDevice {
public:
    virtual ~MetalDevice() = default;
    [[nodiscard]] virtual const std::string& name() const noexcept = 0;
    [[nodiscard]] virtual std::unique_ptr<MetalBuffer> allocate(std::size_t size_bytes) = 0;
    virtual void upload(MetalBuffer& buffer, std::span<const std::byte> source, std::size_t offset) = 0;
    virtual void download(const MetalBuffer& buffer, std::span<std::byte> destination, std::size_t offset) const = 0;
    virtual void vector_add(const MetalBuffer& a, const MetalBuffer& b, MetalBuffer& c, std::size_t element_count) = 0;
    [[nodiscard]] virtual std::shared_ptr<void> submit_vector_add(
        const MetalBuffer& a, const MetalBuffer& b, MetalBuffer& c,
        std::size_t element_count,
        std::function<void(std::exception_ptr, std::optional<double>, std::optional<double>)> completion) = 0;
    virtual void synchronize() = 0;
};

[[nodiscard]] std::unique_ptr<MetalDevice> create_metal_device();

} // namespace gridforge::detail