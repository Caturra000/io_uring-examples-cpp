#pragma once
#include <liburing.h>
#include <memory>
#include <algorithm>
#include <coroutine>
#include <queue>
#include <utility>
#include <thread>
#include <chrono>
#include <algorithm>
#include <ranges>
#include <cassert>
#include "config.h"
#include "utils.h"

struct Task {
    struct promise_type;
    constexpr Task(std::coroutine_handle<promise_type> handle) noexcept: _handle(handle) {}
    ~Task() { if(_handle) _handle.destroy(); }
    auto detach() noexcept { return std::exchange(_handle, {}); }
    // Move ctor only.
    Task(Task &&rhs) noexcept: _handle(rhs.detach()) {}
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task& operator=(Task&&) = delete;
    auto operator co_await() && noexcept;
private:
    std::coroutine_handle<promise_type> _handle;
};

struct Task::promise_type {
    constexpr auto initial_suspend() const noexcept { return std::suspend_always{}; }
    constexpr void return_void() const noexcept { /*exception_ptr...*/ }
    void unhandled_exception() { /*exception_ptr...*/ }
    Task get_return_object() noexcept {
        auto h = std::coroutine_handle<promise_type>::from_promise(*this);
        return {h};
    }
    struct Final_suspend {
        constexpr bool await_ready() const noexcept { return false; }
        auto await_suspend(auto callee) const noexcept {
            auto caller = callee.promise()._caller;
            // Started task (at least once) will kill itself in final_suspend.
            callee.destroy();
            return caller;
        }
        // Never reached.
        constexpr auto await_resume() const noexcept {}
    };
    constexpr auto final_suspend() const noexcept { return Final_suspend{}; }
    void push(std::coroutine_handle<> caller) noexcept { _caller = caller; }

    std::coroutine_handle<> _caller {std::noop_coroutine()};
};

// Multi-task support (rvalue only).
// Examples:
//   GOOD:
//     co_await make_task(...);
//     ////////////////////////////
//     Task task = make_task(...);
//     co_await std::move(task);
//   BAD:
//     Task task = make_task(...); // Compilable but meaningless.
//     co_await task;              // Error. Rejected by compiler.
inline auto Task::operator co_await() && noexcept {
    struct awaiter {
        bool await_ready() const noexcept { return !_handle || _handle.done(); }
        auto await_suspend(std::coroutine_handle<> caller) noexcept {
            _handle.promise().push(caller);
            // Multi-tasks are considered as a single operation in io_contexts.
            return _handle;
        }
        constexpr auto await_resume() const noexcept {}

        std::coroutine_handle<Task::promise_type> _handle;
    };
    return awaiter{detach()};
}

struct Async_user_data {
    io_uring *uring;
    io_uring_sqe *sqe {};
    io_uring_cqe *cqe {};
    // io_contexts may submit before setting up `h` in await_suspend().
    // Therefore:
    // 1. Operations need a check in await_ready().
    // 2. `h` should be initially `std::noop-`, which is safe (and no effect) to resume.
    std::coroutine_handle<> h {std::noop_coroutine()};

    Async_user_data(io_uring *uring) noexcept: uring(uring) {}
};

struct Async_operation {
    constexpr bool await_ready() const noexcept {
        // No allocation error and no eager completion.
        if(user_data.sqe && !user_data.cqe) [[likely]] {
            return false;
        }
        return true;
    }
    void await_suspend(std::coroutine_handle<> h) noexcept {
        user_data.h = h;
    }
    auto await_resume() const noexcept {
        if(!user_data.sqe) [[unlikely]] {
            return -ENOMEM;
        }
        return user_data.cqe->res;
    }
    Async_operation(io_uring *uring, auto uring_prep_fn, auto &&...args) noexcept: user_data(uring) {
        // If !sqe, return -ENOMEM immediately. (await_ready() => true.)
        if((user_data.sqe = io_uring_get_sqe(uring))) [[likely]] {
            uring_prep_fn(user_data.sqe, std::forward<decltype(args)>(args)...);
            // https://man7.org/linux/man-pages/man3/io_uring_cqe_get_data.3.html
            // For Linux v5.15, data must be set AFTER prep_fn();
            // otherwise, io_uring will return an inaccessible CQE.
            // This problem does not exist in Linux v6.1.
            // However, according to the man page,
            // set_data() only needs to be called before submit().
            // Fine, it just works...
            io_uring_sqe_set_data(user_data.sqe, &user_data);
        }
    }

    Async_user_data user_data;
};

inline auto async_operation(io_uring *uring, auto uring_prep_fn, auto &&...args) noexcept {
    return Async_operation(uring, uring_prep_fn, std::forward<decltype(args)>(args)...);
}

// A quite simple io_context.
class Io_context {
public:
    explicit Io_context(io_uring &uring): uring(uring) {}
    Io_context(const Io_context &) = delete;
    Io_context& operator=(const Io_context &) = delete;

    void run() { for(_stop = false; running(); run_once()); }

    // Once = submit + reap.
    template <bool Exactly_once = false>
    void run_once() {
        run_once_prepare();
        auto some = Exactly_once ? take_once() : take_batch();
        namespace views = std::ranges::views;
        for(auto _ : views::iota(0) | views::take(some)) {
            auto op = _operations.front();
            _operations.pop();
            op.resume();
            // Unused.
            [](...){}(_);
        }

        if((_inflight += io_uring_submit(&uring)) == 0) {
            if constexpr (!uring_features::inflight_conflict) {
                hang();
                return;
            }
        }

        // Some cqes are in-flight,
        // even if we currently have no any h.resume().
        // Just continue along the path!

        io_uring_cqe *cqe;
        unsigned head;
        unsigned done = 0;
        // Reap one operation / multiple operations.
        // NOTE: One operation can also generate multiple cqes (awaiters).
        io_uring_for_each_cqe(&uring, head, cqe) {
            done++;
            // For io_uring_prep_cancel().
            if(cqe->res == -ECANCELED) [[unlikely]] continue;
            auto user_data = std::bit_cast<Async_user_data*>(cqe->user_data);
            user_data->cqe = cqe;
            user_data->h.resume();
        }
        done ? io_uring_cq_advance(&uring, done) : hang();

        uring_features::inflight_conflict_workaround(_inflight, done);
        assert(_inflight >= done);
        _inflight -= done;
    }

    // Some observable IO statistics.
    // These APIs are not affected by stop flag.
    auto pending() const { return _operations.size(); }
    auto inflight() const noexcept { return _inflight; }
    bool drained() const { return !pending() && !inflight(); }

    // Only affect the run() interface.
    // The stop flag will be reset upon re-run().
    //
    // Some in-flight operations will be suspended when calling stop().
    // This provides the opportunity to do fire-and-forget tasks.
    //
    // So it is the responsibility of users to ensure the correctness of this function.
    // What users can do if they want to complete all tasks:
    // 1. blocking method: re-run() agagin.
    // 2. non-blocking method: while(!drained()) run_once();
    void stop() noexcept { _stop = true; }
    bool stopped() const { return _stop && !pending(); }
    bool running() const { return !stopped(); }

    friend void co_spawn(Io_context &io_context, Task &&task) {
        io_context._operations.emplace(task.detach());
    }

    // Lock-free for SPSC only.
    // For MPSC, a user-defined lock should be held to prevent contention.
    bool push_to_switch(std::coroutine_handle<> h) noexcept { return _switch_spsc.push(h); }

    // TODO:
    // shutdown();

private:
    void hang() {
        // TODO: config option, event driven.
        constexpr bool ENABLE_BUSY_LOOP = false;
        if constexpr (!ENABLE_BUSY_LOOP) {
            // FIXME: yield() in a single thread makes no sense.
            using namespace std::chrono_literals;
            std::this_thread::sleep_for(1ns);
        }
    }

    size_t take_batch() const {
        constexpr size_t /*same type*/ BATCH_MAX = 32;
        return std::min(BATCH_MAX, _operations.size());
    }

    size_t take_once() const {
        return !_operations.empty();
    }

    void run_once_prepare() noexcept {
        // Pull one coroutine to this running context.
        std::coroutine_handle<> h {};
        _switch_spsc.pop(h);
        if(h) _operations.emplace(h);
    }

    io_uring &uring;
    std::queue<std::coroutine_handle<>> _operations;
    size_t _inflight {};
    bool _stop {false};
    Fifo<std::coroutine_handle<>, 64> _switch_spsc;
    // TODO: work_guard;
};

inline auto async_accept(io_uring *uring, int server_fd,
        sockaddr *addr, socklen_t *addrlen, int flags = 0) noexcept {
    return async_operation(uring,
        io_uring_prep_accept, server_fd, addr, addrlen, flags);
}

inline auto async_accept(io_uring *uring, int server_fd, int flags = 0) noexcept {
    return async_operation(uring,
        io_uring_prep_accept, server_fd, nullptr, nullptr, flags);
}

inline auto async_read(io_uring *uring, int fd, void *buf, size_t n, int flags = 0) noexcept {
    return async_operation(uring,
        io_uring_prep_read, fd, buf, n, flags);
}

inline auto async_write(io_uring *uring, int fd, const void *buf, size_t n, int flags = 0) noexcept {
    return async_operation(uring,
        io_uring_prep_write, fd, buf, n, flags);
}

inline auto async_close(io_uring *uring, int fd) noexcept {
    return async_operation(uring,
        io_uring_prep_close, fd);
}
