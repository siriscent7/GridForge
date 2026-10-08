#include <metal_stdlib>

using namespace metal;

kernel void vector_add(
    device const float* a [[buffer(0)]],
    device const float* b [[buffer(1)]],
    device float* c [[buffer(2)]],
    constant uint& element_count [[buffer(3)]],
    uint index [[thread_position_in_grid]]) {
    if (index < element_count) {
        c[index] = a[index] + b[index];
    }
}