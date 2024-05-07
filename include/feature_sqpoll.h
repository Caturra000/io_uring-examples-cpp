#pragma once
#include <liburing.h>
#include <chrono>
#include <concepts>

template <typename Duration>
concept chrono_duration = requires(Duration d) {
    std::chrono::duration_cast<std::chrono::milliseconds>(d);
};

// https://www.man7.org/linux/man-pages/man2/io_uring_setup.2.html
//
// Note that, when using a ring setup with
// IORING_SETUP_SQPOLL, you never directly call the
// io_uring_enter(2) system call. That is usually taken care
// of by liburing's io_uring_submit(3) function. It
// automatically determines if you are using polling mode or
// not and deals with when your program needs to call
// io_uring_enter(2) without you having to bother about it.
//
// ==========================================================
//
// If the kernel thread is idle for more than sq_thread_idle
// milliseconds, it will set the IORING_SQ_NEED_WAKEUP bit in
// the flags field of the struct io_sq_ring.  When this
// happens, the application must call io_uring_enter(2) to
// wake the kernel thread.  If I/O is kept busy, the kernel
// thread will never sleep.  An application making use of
// this feature will need to guard the io_uring_enter(2) call
// with the following code sequence:
//
//     /*
//      * Ensure that the wakeup flag is read after the tail pointer
//      * has been written. It's important to use memory load acquire
//      * semantics for the flags read, as otherwise the application
//      * and the kernel might not agree on the consistency of the
//      * wakeup flag.
//      */
//     unsigned flags = atomic_load_relaxed(sq_ring->flags);
//     if (flags & IORING_SQ_NEED_WAKEUP)
//         io_uring_enter(fd, 0, 0, IORING_ENTER_SQ_WAKEUP);
//
// ==========================================================
//
// RTFSC:
// https://github.com/axboe/liburing/blob/dcd83d74540f78db9006b5728f281c5bc642eb49/src/queue.c#L364
// https://github.com/axboe/liburing/blob/dcd83d74540f78db9006b5728f281c5bc642eb49/src/queue.c#L17
// io_uring_submit -> __io_uring_submit -> sq_ring_needs_enter
// -> check IORING_SQ_NEED_WAKEUP
//     -> true: goto SYSCALL io_uring_enter();
//     -> false: return immediately.
//
// Conclusion:
// Using io_uring_submit() with SQPOLL mode is OK.
inline auto make_sqpoll_params(chrono_duration auto thread_idle) {
    io_uring_params params {};
    set_sqpoll_params(params, thread_idle);
    return params;
}

// For pre-defined params.
inline void set_sqpoll_params(io_uring_params &params, chrono_duration auto thread_idle) /*noexcept*/ {
    params.flags |= IORING_SETUP_SQPOLL;
    using namespace std::chrono;
    // std::chrono functions are not "noexcept".
    auto normal_duration = duration_cast<milliseconds>(thread_idle);
    params.sq_thread_idle = normal_duration.count();
}
