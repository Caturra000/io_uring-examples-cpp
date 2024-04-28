#pragma once
#include <tuple>
#include "coroutine.h"

struct Async_multishot_operation: private Async_operation {
    using Base = Async_operation;
    using Base::Base;
    using Base::await_suspend;

    constexpr bool await_ready() const noexcept {
        if(user_data.no_resume && user_data.cqe) {
            return true;
        }
        return Base::await_ready();
    }

    auto await_resume() noexcept {
        auto res = Base::await_resume();
        auto &cqe = user_data.cqe;
        if(!cqe || !(cqe->flags & IORING_CQE_F_MORE)) [[unlikely]] {
            user_data.uring = nullptr;
        }
        cqe = nullptr;
        return res;
    }

    // Is it co_await-able?
    operator bool() const noexcept {
        return !!user_data.uring;
    }
};

inline auto async_multishot_operation(io_uring *uring, auto uring_prep_fn, auto &&...args) noexcept {
    return Async_multishot_operation(uring, uring_prep_fn, std::forward<decltype(args)>(args)...);
}

inline auto async_multishot_accept(io_uring *uring, int server_fd, int flags = 0) {
    return async_multishot_operation(uring,
        io_uring_prep_multishot_accept, server_fd, nullptr, nullptr, flags);
}
