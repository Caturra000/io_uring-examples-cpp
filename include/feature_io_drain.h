#pragma once
#include <tuple>
#include "coroutine.h"

struct Async_drain_operation: private Async_operation {
    using Base = Async_operation;
    using Base::Base;
    using Base::await_ready;
    using Base::await_resume;

    auto await_suspend(std::coroutine_handle<> h) noexcept {
        // When this flag is specified, the SQE will not be started
        // before previously submitted SQEs have completed, and new
        // SQEs will not be started before this one completes.
        user_data.sqe->flags |= IOSQE_IO_DRAIN;
        return Base::await_suspend(h);
    }
};

inline auto async_drain_operation(io_uring *uring, auto uring_prep_fn, auto &&...args) noexcept {
    return Async_drain_operation(uring, uring_prep_fn, std::forward<decltype(args)>(args)...);
}

inline auto async_drain_accept(io_uring *uring, int server_fd, int flags = 0) {
    return async_drain_operation(uring,
        io_uring_prep_accept, server_fd, nullptr, nullptr, flags);
}
