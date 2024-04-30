#pragma once
#include <liburing.h>
#include <type_traits>
#include "coroutine.h"

template <bool Hard = false>
struct Sync_link_operation: public /* for implicit conversion */ Async_operation {
    template <typename T>
    Sync_link_operation(T &&lhs, Async_operation &&rhs) noexcept
            : Async_operation(static_cast<Async_operation&&>(rhs)) {
        auto &first_sqe = lhs.user_data.sqe;
        if constexpr (!Hard) {
            // That next SQE will not be started before the previous request completes.
            // A chain of SQEs will be broken if any request in that chain ends in error.
            first_sqe->flags |= IOSQE_IO_LINK;
        } else {
            // Like IOSQE_IO_LINK , except the links aren't severed if an
            // error or unexpected result occurs.
            first_sqe->flags |= IOSQE_IO_HARDLINK;
        }

        // No resume in io_contexts.
        first_sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
        // Reset userdata address.
        io_uring_sqe_set_data(user_data.sqe, &user_data);
    }
};

// & -> link.
[[nodiscard]]
inline auto operator&(Async_operation &&first, Async_operation &&second) noexcept {
    return Sync_link_operation(std::move(first), std::move(second));
}

template <bool H>
[[nodiscard]]
inline auto operator&(Sync_link_operation<H> &&first, Async_operation &&second) noexcept {
    return Sync_link_operation(std::move(first), std::move(second));
}

// | -> hardlink.
[[nodiscard]]
inline auto operator|(Async_operation &&first, Async_operation &&second) noexcept {
    return Sync_link_operation<true>(std::move(first), std::move(second));
}

template <bool H>
[[nodiscard]]
inline auto operator|(Sync_link_operation<H> &&first, Async_operation &&second) noexcept {
    return Sync_link_operation<true>(std::move(first), std::move(second));
}
