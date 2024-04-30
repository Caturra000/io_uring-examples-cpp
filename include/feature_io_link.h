#pragma once
#include <liburing.h>
#include <type_traits>
#include "coroutine.h"

template <bool Hard = false>
struct Sync_link_operation: public /* for implicit conversion */ Async_operation {
    template <std::convertible_to<Async_operation> Operation>
    Sync_link_operation(Operation &&previous, Async_operation &&next) noexcept
            : Async_operation(static_cast<Async_operation&&>(next)) {
        auto &previous_sqe = previous.user_data.sqe;
        if constexpr (!Hard) {
            // That next SQE will not be started before the previous request completes.
            // A chain of SQEs will be broken if any request in that chain ends in error.
            previous_sqe->flags |= IOSQE_IO_LINK;
        } else {
            // Like IOSQE_IO_LINK , except the links aren't severed if an
            // error or unexpected result occurs.
            previous_sqe->flags |= IOSQE_IO_HARDLINK;
        }
        // No resume in io_contexts.
        previous_sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;
        // Reset userdata address.
        io_uring_sqe_set_data(user_data.sqe, &user_data);
    }
};

// & -> link.
// | -> hardlink.
// (Async_operation, Async_operation) -> Sync_link_operation.
// (Sync_link_operation, Async_operation) -> Sync_link_operation.

[[nodiscard]]
inline auto operator&(Async_operation &&previous, Async_operation &&next) noexcept {
    return Sync_link_operation<false>(std::move(previous), std::move(next));
}

template <bool H>
[[nodiscard]]
inline auto operator&(Sync_link_operation<H> &&previous, Async_operation &&next) noexcept {
    return Sync_link_operation<false>(std::move(previous), std::move(next));
}

[[nodiscard]]
inline auto operator|(Async_operation &&previous, Async_operation &&next) noexcept {
    return Sync_link_operation<true>(std::move(previous), std::move(next));
}

template <bool H>
[[nodiscard]]
inline auto operator|(Sync_link_operation<H> &&previous, Async_operation &&next) noexcept {
    return Sync_link_operation<true>(std::move(previous), std::move(next));
}
