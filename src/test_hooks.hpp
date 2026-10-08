#pragma once

#include "gridforge/gridforge.hpp"

#include <functional>

namespace gridforge::detail {

struct TestAccess {
    static Event enqueue_test_task(Stream& stream, std::function<void()> task);
    static Event enqueue_deferred_task(
        Stream& stream,
        std::function<std::shared_ptr<void>(std::function<void(std::exception_ptr)>)> submit);
    static void synchronize_after_snapshot(Stream& stream, std::function<void()> snapshot_taken);
};

} // namespace gridforge::detail
