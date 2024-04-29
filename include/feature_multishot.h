#pragma once
#include <tuple>
#include "coroutine.h"

// NOTE:
// co_await async_multishot_operation is for special usage.
// Users can only co_await one awaiter in the same scope:
// Example1:
//   for(auto multishot_op = async_multishot_operation;;) {
//       auto ret = co_await multishot_op;
//       auto ret2 = co_await ...; // ERROR.
//       co_spawn(...);            // OK.
//   } // Multishot operation will be cacnel in destructor.
// Example2:
//   for(auto multishot_op = async_multishot_operation;;) {
//       auto ret = co_await multishot_op;
//   }
//   auto ret2 = co_await ...; // OK.
struct Async_multishot_operation: private Async_operation {
    using Base = Async_operation;
    using Base::Base;
    using Base::await_ready;
    using Base::await_suspend;

    auto await_resume() noexcept {
        auto res = Base::await_resume();
        auto cqe = user_data.cqe;
        if(!cqe || !(cqe->flags & IORING_CQE_F_MORE)) [[unlikely]] {
            user_data.uring = nullptr;
        }
        return res;
    }

    // Is it co_await-able?
    operator bool() const noexcept {
        return !!user_data.uring;
    }

    // By default, multi-shot requests will remain active until:
    // They get canceled explicitly by the application (eg using io_uring_prep_cancel() and friends), or
    // The request itself experiences an error.
    ~Async_multishot_operation() {
        // Experience an error before await_resume().
        if(!user_data.uring) return;

        auto sqe = io_uring_get_sqe(user_data.uring);
        // FIXME: reserved sqe.
        if(!sqe) return;

        // No resume in io_context.
        sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
        // The first request matching the `user_data` will be canceled.
        // NOTE: user_data will be invalid after dtor, but liburing just compares its address.
        io_uring_prep_cancel(sqe, &user_data, 0);
        io_uring_submit(user_data.uring);
    }
};

inline auto async_multishot_operation(io_uring *uring, auto uring_prep_fn, auto &&...args) noexcept {
    return Async_multishot_operation(uring, uring_prep_fn, std::forward<decltype(args)>(args)...);
}

inline auto async_multishot_accept(io_uring *uring, int server_fd, int flags = 0) {
    return async_multishot_operation(uring,
        io_uring_prep_multishot_accept, server_fd, nullptr, nullptr, flags);
}
