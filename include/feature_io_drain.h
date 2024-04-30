#pragma once
#include "coroutine.h"

struct Async_drain_operation: private Async_operation {
    using Base = Async_operation;
    using Base::await_ready;
    using Base::await_suspend;
    using Base::await_resume;

    Async_drain_operation(auto &&...args) noexcept: Base(std::forward<decltype(args)>(args)...) {
        // When this flag is specified, the SQE will not be started
        // before previously submitted SQEs have completed, and new
        // SQEs will not be started before this one completes.
        user_data.sqe->flags |= IOSQE_IO_DRAIN;
    }
};

inline auto async_drain_operation(io_uring *uring, auto uring_prep_fn, auto &&...args) noexcept {
    return Async_drain_operation(uring, uring_prep_fn, std::forward<decltype(args)>(args)...);
}

inline auto async_drain_accept(io_uring *uring, int server_fd, int flags = 0) noexcept {
    return async_drain_operation(uring,
        io_uring_prep_accept, server_fd, nullptr, nullptr, flags);
}
