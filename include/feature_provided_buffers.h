#pragma once
#include <liburing.h>
#include <ranges>
#include <memory>
#include "coroutine.h"
#include "utils.h"

// nentries: the number of entries requested in the buffer ring. MUST be a power-of 2 in size.
auto make_provided_buffers(io_uring *uring, int bgid, unsigned int nentries, size_t per_buffer_size) {
    int ret;

    // https://github.com/axboe/liburing/blob/f7dcc1ea60819475dffd3a45059e16f04381bee7/src/setup.c#L660
    // io_uring_setup_buf_ring() = io_uring_register_buf_ring() + io_uring_buf_ring_init().
    io_uring_buf_ring *buf_ring = io_uring_setup_buf_ring(uring, nentries, bgid, {} /*flags is currently unused in io_uring*/, &ret);

    // FIXME: throw?
    if(!buf_ring) {
        ret | nofail("io_uring_setup_buf_ring");
    }

    // io_uring_free_buf_ring() = io_uring_unregister_buf_ring() + munmap().
    auto buf_ring_cleanup = defer([&](...) { io_uring_free_buf_ring(uring, buf_ring, nentries, 0); });

    auto buffers = std::make_unique<char[]>(per_buffer_size * nentries);
    auto buffer = [=, base = buffers.get()](size_t index) { return &base[per_buffer_size * index]; };

    for(auto i : std::views::iota(0) | std::views::take(nentries)) {
        // `bid` is the buffer ID, which will be returned in the CQE.
        // `buf_offset` is the offset to insert at from the current tail.
        io_uring_buf_ring_add(buf_ring, buffer(i), per_buffer_size, i /*bid*/, io_uring_buf_ring_mask(nentries), i /*buf_offset*/);
    }
    // Commit `count` previously added buffers, making them visible to the kernel and hence consumable.
    io_uring_buf_ring_advance(buf_ring, nentries /*count*/);

    return std::tuple(buf_ring, std::move(buffers), std::move(buffer), std::move(buf_ring_cleanup));
}

struct Async_provided_buffers_operations: private Async_operation {
    using Base = Async_operation;
    using Base::await_ready;
    using Base::await_suspend;

    // Return {res, bid}.
    std::tuple<int, unsigned int> await_resume() const noexcept {
        auto res = Base::await_resume();
        if(res < 0) {
            // Users should not look at the `bid` field on errors.
            return {res, {}};
        }
        auto cqe = user_data.cqe;
        // On successful completion of the IO request, the CQE flags field
        // will have IORING_CQE_F_BUFFER set and the selected buffer ID will be
        // indicated by the upper 16-bits of the flags field.
        if(!(cqe->flags & IORING_CQE_F_BUFFER)) {
            return {res, {}};
        }
        // IORING_CQE_BUFFER_SHIFT == 16.
        auto bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
        // NOTE: buffers rejoin by users.
        // Once a buffer has been used, it is no longer
        // available in the kernel pool. The application MUST re-
        // register the given buffer again when it is ready to
        // recycle it (eg has completed using it).
        return {cqe->res, bid};
    }

    Async_provided_buffers_operations(io_uring_buf_ring *buf_ring, int bgid, auto &&...args) noexcept
            : Base(std::forward<decltype(args)>(args)...), buf_ring(buf_ring) {
        // If set, and if the request types supports it, select an IO
        // buffer from the indicated buffer group. This can be used
        // with requests that read or receive data from a file or
        // socket, where buffer selection is deferred until the
        // kernel is ready to transfer data, instead of when the IO
        // is originally submitted. The application must also set the
        // buf_group field in the SQE, indicating which previously
        // registered buffer group to select a buffer from.
        user_data.sqe->flags |= IOSQE_BUFFER_SELECT;
        user_data.sqe->buf_group = bgid;
    }
    io_uring_buf_ring *buf_ring;
};

// TODO: unstable API.
inline auto use_provided_buffers(io_uring *uring, io_uring_buf_ring *buf_ring, int bgid) noexcept {
    return std::tuple(uring, buf_ring, bgid);
}

// TODO: unstable API.
// Simliar to Asio's completion token.
// But in this implementation, we pass the token as the first parameter.
using use_provided_buffers_t = std::tuple<io_uring*, io_uring_buf_ring*, int>;

// Simply to assist in simplifying the code.
inline auto make_buffer_helper(auto buffer_finder, size_t buffer_size, unsigned int buf_ring_capacity) noexcept {
    return std::tuple(buffer_finder, buffer_size, buf_ring_capacity);
}

inline void buffer_rejoin(const use_provided_buffers_t &token, const auto &buffer_helper, int bid) noexcept {
    const auto &[_, buf_ring, _2] = token;
    const auto &[buffer_finder, buffer_size, buf_ring_capacity] = buffer_helper;
    const auto buf = buffer_finder(bid);
    const auto mask = io_uring_buf_ring_mask(buf_ring_capacity);
    io_uring_buf_ring_add(buf_ring, buf, buffer_size, bid, mask, 0);
    io_uring_buf_ring_advance(buf_ring, 1);
}

// Simliar to Asio's completion signature.
// But in this implementation, we pass the token as the first parameter.
// That is: [token, sync_read_signature...]
inline auto async_read(use_provided_buffers_t pb, int fd, void *buf, size_t n, int flags = 0) noexcept {
    // https://man7.org/linux/man-pages/man3/io_uring_prep_provide_buffers.3.html
    // ...If buffer selection
    // is used for a request, no buffer should be provided in the
    // address field. Instead, the group ID is set to match one that was
    // previously provided to the kernel. The kernel will then select a
    // buffer from this group for the IO operation. On successful
    // completion of the IO request, the CQE flags field will have
    // IORING_CQE_F_BUFFER set and the selected buffer ID will be
    // indicated by the upper 16-bits of the flags field.
    assert(buf == nullptr);
    [](...){}(buf);
    auto [uring, buf_ring, bid] = pb;
    return Async_provided_buffers_operations(buf_ring, bid, uring,
        io_uring_prep_read, fd, nullptr, n, flags);
}
