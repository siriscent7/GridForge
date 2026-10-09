#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gridforge/gridforge.hpp"
#include "metal_bridge.hpp"
#include "metal_shader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace gridforge::detail {
namespace {

std::string error_description(NSError* error, const char* fallback) {
    if (error == nil || error.localizedDescription == nil) {
        return fallback;
    }
    const char* text = error.localizedDescription.UTF8String;
    return text == nullptr ? fallback : text;
}

class AppleMetalBuffer final : public MetalBuffer {
public:
    AppleMetalBuffer(id<MTLBuffer> buffer, std::size_t bytes) : buffer_(buffer), bytes_(bytes) {}

    [[nodiscard]] std::size_t size_bytes() const noexcept override { return bytes_; }
    [[nodiscard]] id<MTLBuffer> native() const noexcept { return buffer_; }

private:
    __strong id<MTLBuffer> buffer_;
    std::size_t bytes_;
};

class AppleMetalSubmission final {
public:
    explicit AppleMetalSubmission(id<MTLCommandBuffer> command) : command_(command) {}
private:
    __strong id<MTLCommandBuffer> command_;
};

class AppleMetalDevice final : public MetalDevice {
public:
    AppleMetalDevice() : device_(MTLCreateSystemDefaultDevice()) {
        if (device_ == nil) {
            throw BackendUnavailable("Metal backend is unavailable: no Metal device was discovered.");
        }
        const char* device_name = device_.name.UTF8String;
        name_ = device_name == nullptr ? "Unknown Metal device" : device_name;

        NSError* error = nil;
        NSString* source = [NSString stringWithUTF8String:vector_add_shader_source];
        if (source == nil) {
            throw std::runtime_error("Metal shader source is not valid UTF-8.");
        }
        id<MTLLibrary> library = [device_ newLibraryWithSource:source options:nil error:&error];
        if (library == nil) {
            throw std::runtime_error("Metal vector-add shader compilation failed: " + error_description(error, "unknown compiler error"));
        }
        id<MTLFunction> function = [library newFunctionWithName:@"vector_add"];
        if (function == nil) {
            throw std::runtime_error("Metal shader library does not contain the vector_add kernel.");
        }
        pipeline_ = [device_ newComputePipelineStateWithFunction:function error:&error];
        if (pipeline_ == nil) {
            throw std::runtime_error("Metal vector-add pipeline creation failed: " + error_description(error, "unknown pipeline error"));
        }
        queue_ = [device_ newCommandQueue];
        if (queue_ == nil) {
            throw std::runtime_error("Metal command queue creation failed.");
        }
    }

    [[nodiscard]] const std::string& name() const noexcept override { return name_; }

    [[nodiscard]] std::unique_ptr<MetalBuffer> allocate(std::size_t size_bytes) override {
        @autoreleasepool {
        if (size_bytes == 0) {
            return std::make_unique<AppleMetalBuffer>(nil, 0);
        }
        id<MTLBuffer> buffer = [device_ newBufferWithLength:size_bytes options:MTLResourceStorageModeShared];
        if (buffer == nil) {
            throw std::bad_alloc();
        }
        return std::make_unique<AppleMetalBuffer>(buffer, size_bytes);
        }
    }

    void upload(MetalBuffer& buffer, std::span<const std::byte> source, std::size_t offset) override {
        @autoreleasepool {
        auto& metal_buffer = checked(buffer);
        std::memcpy(static_cast<std::byte*>(metal_buffer.native().contents) + offset, source.data(), source.size());
        }
    }

    void download(const MetalBuffer& buffer, std::span<std::byte> destination, std::size_t offset) const override {
        @autoreleasepool {
        const auto& metal_buffer = checked(buffer);
        std::memcpy(destination.data(), static_cast<const std::byte*>(metal_buffer.native().contents) + offset, destination.size());
        }
    }

    void vector_add(const MetalBuffer& a, const MetalBuffer& b, MetalBuffer& c, std::size_t element_count) override {
        @autoreleasepool {
        const auto& left = checked(a);
        const auto& right = checked(b);
        auto& output = checked(c);
        if (element_count > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("Metal vector-add element count exceeds the shader's uint range.");
        }
        const std::uint32_t count = static_cast<std::uint32_t>(element_count);
        const NSUInteger max_threads = pipeline_.maxTotalThreadsPerThreadgroup;
        const NSUInteger execution_width = pipeline_.threadExecutionWidth;
        NSUInteger group_width = std::min<NSUInteger>(max_threads, 256);
        if (execution_width > 0 && group_width >= execution_width) {
            group_width = (group_width / execution_width) * execution_width;
        }
        if (group_width == 0) {
            throw std::runtime_error("Metal reported no legal threads per threadgroup.");
        }

        id<MTLCommandBuffer> command = [queue_ commandBuffer];
        if (command == nil) {
            throw std::runtime_error("Metal failed to create a command buffer.");
        }
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (encoder == nil) {
            throw std::runtime_error("Metal failed to create a compute command encoder.");
        }
        [encoder setComputePipelineState:pipeline_];
        [encoder setBuffer:left.native() offset:0 atIndex:0];
        [encoder setBuffer:right.native() offset:0 atIndex:1];
        [encoder setBuffer:output.native() offset:0 atIndex:2];
        [encoder setBytes:&count length:sizeof(count) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake(element_count, 1, 1)
           threadsPerThreadgroup:MTLSizeMake(group_width, 1, 1)];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            throw std::runtime_error("Metal vector-add command failed: " + error_description(command.error, "command buffer did not complete"));
        }
        }
    }

    [[nodiscard]] std::shared_ptr<void> submit_vector_add(
        const MetalBuffer& a, const MetalBuffer& b, MetalBuffer& c,
        std::size_t element_count,
        std::function<void(std::exception_ptr, std::optional<double>, std::optional<double>)> completion) override {
        @autoreleasepool {
            if (element_count == 0) {
            completion({}, std::nullopt, std::nullopt);
                return {};
            }
            const auto& left = checked(a);
            const auto& right = checked(b);
            auto& output = checked(c);
            if (element_count > std::numeric_limits<std::uint32_t>::max()) {
                throw std::length_error("Metal vector-add element count exceeds the shader's uint range.");
            }
            const std::uint32_t count = static_cast<std::uint32_t>(element_count);
            const NSUInteger max_threads = pipeline_.maxTotalThreadsPerThreadgroup;
            const NSUInteger execution_width = pipeline_.threadExecutionWidth;
            NSUInteger group_width = std::min<NSUInteger>(max_threads, 256);
            if (execution_width > 0 && group_width >= execution_width) {
                group_width = (group_width / execution_width) * execution_width;
            }
            if (group_width == 0) throw std::runtime_error("Metal reported no legal threads per threadgroup.");

            id<MTLCommandBuffer> command = [queue_ commandBuffer];
            if (command == nil) throw std::runtime_error("Metal failed to create an asynchronous command buffer.");
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            if (encoder == nil) throw std::runtime_error("Metal failed to create an asynchronous compute encoder.");
            [encoder setComputePipelineState:pipeline_];
            [encoder setBuffer:left.native() offset:0 atIndex:0];
            [encoder setBuffer:right.native() offset:0 atIndex:1];
            [encoder setBuffer:output.native() offset:0 atIndex:2];
            [encoder setBytes:&count length:sizeof(count) atIndex:3];
            [encoder dispatchThreads:MTLSizeMake(element_count, 1, 1)
               threadsPerThreadgroup:MTLSizeMake(group_width, 1, 1)];
            [encoder endEncoding];

            auto submission = std::make_shared<AppleMetalSubmission>(command);

            [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
                @autoreleasepool {
                    try {
                        if (completed.status == MTLCommandBufferStatusCompleted) {
                            const double gpu_start = completed.GPUStartTime;
                            const double gpu_end = completed.GPUEndTime;
                            if (std::isfinite(gpu_start) && std::isfinite(gpu_end) &&
                                gpu_start > 0.0 && gpu_end >= gpu_start && gpu_end > 0.0) {
                                completion({}, gpu_start, gpu_end);
                            } else {
                                completion({}, std::nullopt, std::nullopt);
                            }
                        } else {
                            const auto message = "Metal asynchronous vector-add command failed: " +
                                error_description(completed.error, "command buffer did not complete");
                            completion(std::make_exception_ptr(std::runtime_error(message)), std::nullopt, std::nullopt);
                        }
                    } catch (...) {
                        try {
                            completion(std::current_exception(), std::nullopt, std::nullopt);
                        } catch (...) {
                            // Runtime completion forwarding is noexcept and does not allocate.
                        }
                    }
                }
            }];
            [command commit];
            return submission;
        }
    }

    void synchronize() override {
        // Runtime-level stream synchronization waits for callback retirement.
    }

private:
    static AppleMetalBuffer& checked(MetalBuffer& buffer) {
        auto* result = dynamic_cast<AppleMetalBuffer*>(&buffer);
        if (result == nullptr) {
            throw std::invalid_argument("Metal operation received a buffer from another backend.");
        }
        return *result;
    }

    static const AppleMetalBuffer& checked(const MetalBuffer& buffer) {
        auto* result = dynamic_cast<const AppleMetalBuffer*>(&buffer);
        if (result == nullptr) {
            throw std::invalid_argument("Metal operation received a buffer from another backend.");
        }
        return *result;
    }

    __strong id<MTLDevice> device_;
    __strong id<MTLCommandQueue> queue_;
    __strong id<MTLComputePipelineState> pipeline_;
    std::string name_;
};

} // namespace

std::unique_ptr<MetalDevice> create_metal_device() {
    @autoreleasepool {
    return std::make_unique<AppleMetalDevice>();
    }
}

} // namespace gridforge::detail