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
#include "utils.h"

struct Task {
    struct promise_type;
    constexpr Task(std::coroutine_handle<promise_type> handle) noexcept: _handle(handle) {}
    ~Task() { if(_handle && !_co_awaited) _handle.destroy(); }
    auto detach() noexcept { return std::exchange(_handle, {}); }
    // Move ctor only.
    Task(const Task&) = delete;
    Task(Task &&rhs): _handle(rhs.detach()), _co_awaited(rhs._co_awaited /*safe*/) {}
    Task& operator=(const Task&) = delete;
    Task& operator=(Task&&) = delete;
    auto operator co_await() && noexcept;
private:
    std::coroutine_handle<promise_type> _handle;
    bool _co_awaited {false};
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
    _co_awaited = true;
    return awaiter{_handle};
}

struct Async_user_data {
    io_uring *uring;
    io_uring_sqe *sqe;
    io_uring_cqe *cqe;
    std::coroutine_handle<> h;
};

// Currently `Result` is unused.
template <typename Result>
struct Async_operation {
    constexpr bool await_ready() const noexcept {
        if(!user_data.sqe) [[unlikely]] {
            return true;
        }
        return false;
    }
    void await_suspend(std::coroutine_handle<> h) {
        user_data.h = h;
        io_uring_sqe_set_data(user_data.sqe, &user_data);
        // Eager? Lazy? SQPOLL?
        // io_uring_submit(user_data.uring);
    }
    // TODO: Don't return cqe->res directly.
    auto await_resume() const noexcept {
        if(!user_data.sqe) [[unlikely]] {
            return -ENOMEM;
        }
        return user_data.cqe->res;
    }
    Async_operation(io_uring *uring, auto uring_prep_fn, auto &&...args) {
        user_data.uring = uring;
        // If !sqe, return -ENOMEM immediately. (await_ready() => true.)
        if((user_data.sqe = io_uring_get_sqe(uring))) [[likely]] {
            uring_prep_fn(user_data.sqe, std::forward<decltype(args)>(args)...);
        }
    }

    Async_user_data user_data;
};

inline auto async_operation(io_uring *uring, auto uring_prep_fn, auto &&...args) {
    using Result = std::invoke_result_t<decltype(uring_prep_fn), io_uring_sqe*, decltype(args)...>;
    return Async_operation<Result>(uring, uring_prep_fn, std::forward<decltype(args)>(args)...);
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
        auto loop_count = Exactly_once ? runtime_once() : runtime_plug();
        for(auto _ : std::views::iota(0, loop_count)) {
            auto h = _operations.front();
            _operations.pop();
            h.resume();
            // Unused.
            [](...){}(_);
        }

        // TODO: SQPOLL.
        if((_inflight += io_uring_submit(&uring)) == 0) {
            hang();
            return;
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
            auto user_data = std::bit_cast<Async_user_data*>(cqe->user_data);
            user_data->cqe = cqe;
            user_data->h.resume();
        }
        done ? io_uring_cq_advance(&uring, done) : hang();

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

    int runtime_plug() const {
        constexpr size_t /*same type*/ PLUG_MAX = 32;
        return std::min(PLUG_MAX, _operations.size());
    }

    int runtime_once() const {
        return !_operations.empty();
    }

    io_uring &uring;
    std::queue<std::coroutine_handle<>> _operations;
    size_t _inflight {};
    bool _stop {false};
    // TODO: work_guard;
};

inline auto async_accept(io_uring *uring, int server_fd,
        sockaddr *addr, socklen_t *addrlen, int flags = 0) {
    return async_operation(uring,
        io_uring_prep_accept, server_fd, addr, addrlen, flags);
}

inline auto async_accept(io_uring *uring, int server_fd, int flags = 0) {
    return async_operation(uring,
        io_uring_prep_accept, server_fd, nullptr, nullptr, flags);
}

inline auto async_read(io_uring *uring, int fd, void *buf, size_t n, int flags = 0) {
    return async_operation(uring,
        io_uring_prep_read, fd, buf, n, flags);
}

inline auto async_write(io_uring *uring, int fd, const void *buf, size_t n, int flags = 0) {
    return async_operation(uring,
        io_uring_prep_write, fd, buf, n, flags);
}

inline auto async_close(io_uring *uring, int fd) {
    return async_operation(uring,
        io_uring_prep_close, fd);
}
