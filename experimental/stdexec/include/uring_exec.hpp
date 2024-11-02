#pragma once
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>
#include <tuple>
#include <liburing.h>
#include <stdexec/execution.hpp>
#include <exec/async_scope.hpp>

// Operations in stdexec are required to be address-stable.
struct immovable {
    immovable() = default;
    immovable(immovable &&) = delete;
};

struct io_uring_exec: immovable {
    io_uring_exec(size_t uring_entries, int uring_flags = 0): _head{{}, {}} {
        if(int err = io_uring_queue_init(uring_entries, &_underlying_uring, uring_flags)) {
            throw std::system_error(-err, std::system_category());
        }
    }

    ~io_uring_exec() {
        io_uring_queue_exit(&_underlying_uring);
    }

    // Per-object vtable for (receiver) type erasure.
    template <typename ...>
    struct make_vtable;

    template <typename Ret, typename ...Args>
    struct make_vtable<Ret(Args...)> {
        Ret (*complete)(Args...);
        // Ret (*ready)(Args...);
    };

    // All the tasks are asynchronous.
    // The `task` struct is queued by a user-space intrusive queue.
    // NOTE: The io_uring-specified task is queued by an interal ring of io_uring.
    struct task: immovable {
        using vtable = make_vtable<void(task*)>;
        vtable vtab;
        task *next {nullptr};
    };

    // Required by stdexec.
    template <stdexec::receiver Receiver>
    struct operation: task {
        using operation_state_concept = stdexec::operation_state_t;

        void start() noexcept {
            uring->push(this);
        }

        inline constexpr static vtable this_vtable {
            .complete = [](task *_self) noexcept {
                auto self = static_cast<operation*>(_self);
                stdexec::set_value(std::move(self->receiver));
            }
        };

        Receiver receiver;
        io_uring_exec *uring;

    };

    // Required by stdexec.
    struct sender {
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
                                        stdexec::set_value_t(),
                                        stdexec::set_error_t(std::exception_ptr),
                                        stdexec::set_stopped_t()>;
        template <stdexec::receiver Receiver>
        operation<Receiver> connect(Receiver receiver) noexcept {
            return {{{}, operation<Receiver>::this_vtable}, std::move(receiver), uring};
        }
        io_uring_exec *uring;
    };

    // Required by stdexec.
    struct scheduler {
        auto operator<=>(const scheduler &) const=default;
        sender schedule() noexcept { return {uring}; }
        io_uring_exec *uring;
    };

    scheduler get_scheduler() noexcept { return {this}; }

    // External structured callbacks support.
    struct io_uring_exec_operation_base: immovable {
        using result_t = decltype(std::declval<io_uring_cqe>().res);
        using vtable = make_vtable<void(io_uring_exec_operation_base*, result_t)>;
        vtable vtab;
    };

    struct run_policy {
        // Informal forward progress guarantees.
        // NOTES:
        // * These are exclusive flags, but using bool (not enum) for simplification.
        // * `weakly_concurrent` is not a C++ standard part, which can make progress eventually
        //   with lower overhead compared to `concurrent`, provided it is used properly.
        // * `parallel` (which makes progress per `step`) is NOT supported for IO operations.
        bool concurrent {true};         // which requires that a thread makes progress eventually.
        bool weakly_parallel {false};   // which does not require that the thread makes progress.
        bool weakly_concurrent {false}; // which requires that a thread may make progress eventually.

        // Event handling.
        // Any combination is welcome.
        bool launch {true};
        bool submit {true};
        bool iodone {true};

        // Behavior details.
        bool busyloop {false};          // No yield.
        bool quitable {false};          // `concurrent` runs infinitely by default.
        bool lockless {false};          // (WIP) Some intrusive queues are within the same thread.
        bool realtime {false};          // (WIP) No delayed process.
    };

    template <run_policy policy = {}>
    void run() {
        // Progress.
        task *first_task {};        // For empty queue detection.
        size_t submitted {};        // For `io_uring_submit`.
        size_t done {};             // For `io_uring_for_each_cqe`.

        // (WIP)
        // Delayed process to minimize the lock acquisition.
        // Finally, we synchronize `_inflight` with `all_done`.
        size_t all_done {};         // Push value (\sum `done`).
        size_t local_inflight {};   // Pull value.

        // TODO: stop_token.
        for(;;) {
            if constexpr (policy.launch) {
                auto [first, last] = move_task_queue();
                if(first) for(auto op = first;; op = op->next) {
                    op->vtab.complete(op);
                    if(op == last) break;
                }
                first_task = first;
            }

            // Need to push/pull (local_)inflight value if there is no submission policy.
            if constexpr (policy.submit) {
                std::lock_guard guard {_submit_mutex};
                submitted = io_uring_submit(&_underlying_uring);
                _inflight += submitted;
                local_inflight = _inflight;
                // When escaping from this scope, we can't access _inflight without mutex.
                // Use local_inflight instead.
            }

            if constexpr (policy.iodone) {
                io_uring_cqe *cqe;
                unsigned head;
                done = 0;
                // Reap one operation / multiple operations.
                // NOTE: One sqe can generate multiple cqes.
                io_uring_for_each_cqe(&_underlying_uring, head, cqe) {
                    done++;
                    using uop = io_uring_exec_operation_base;
                    auto uring_op = std::bit_cast<uop*>(cqe->user_data);
                    uring_op->vtab.complete(uring_op, cqe->res);
                }
                if(done) {
                    all_done += done;
                    io_uring_cq_advance(&_underlying_uring, done);
                }
            }

            if constexpr (policy.weakly_parallel) {
                return;
            }

            bool any_progress = false;
            if constexpr (policy.launch) any_progress |= bool(first_task);
            if constexpr (policy.submit) any_progress |= bool(submitted);
            if constexpr (policy.iodone) any_progress |= bool(done);

            if constexpr (policy.weakly_concurrent) {
                if(any_progress) return;
            }

            if constexpr (!policy.busyloop) {
                if(any_progress) std::this_thread::yield();
            }

            if constexpr (policy.quitable) {
                if(!any_progress && !local_inflight) return;
            }
        }
    }

    void push(task *op) noexcept {
        std::lock_guard _{_tail_mutex};
        std::unique_lock may_own {_head_mutex, std::defer_lock};
        if(_tail == &_head) may_own.lock();
        _tail = _tail->next = op;
    }

    // No pop(), just move.
    std::pair<task*, task*> move_task_queue() noexcept {
        // Don't use std::scoped_lock;
        // its ordering algorithm is implementation-defined.
        std::lock_guard _1 {_tail_mutex};
        std::lock_guard _2 {_head_mutex};
        auto first = std::exchange(_head.next, nullptr);
        auto last = std::exchange(_tail, &_head);
        return {first, last};
    }

    // See the comments on `coroutine.h` and `config.h`.
    size_t /*_inaccurate*/ _inflight {};

    task _head, *_tail{&_head};
    std::mutex _head_mutex, _tail_mutex;
    std::mutex _submit_mutex;
    io_uring _underlying_uring;
};

template <auto F, stdexec::receiver Receiver, typename ...Args>
struct io_uring_exec_operation: io_uring_exec::io_uring_exec_operation_base {
    using operation_state_concept = stdexec::operation_state_t;
    io_uring_exec_operation(Receiver receiver,
                                       io_uring_exec *uring,
                                       std::tuple<Args...> args) noexcept
        : io_uring_exec_operation_base{{}, this_vtable},
          receiver(std::move(receiver)),
          uring(uring),
          args(std::move(args)) {}

    void start() noexcept {
        std::unique_lock guard {uring->_submit_mutex};
        if(auto sqe = io_uring_get_sqe(&uring->_underlying_uring)) [[likely]] {
            io_uring_sqe_set_data(sqe, static_cast<io_uring_exec_operation_base*>(this));
            std::apply(F, std::tuple_cat(std::tuple(sqe), std::move(args)));
        } else {
            guard.unlock();
            // RETURN VALUE
            // io_uring_get_sqe(3)  returns  a  pointer  to the next submission
            // queue event on success and NULL on failure. If NULL is returned,
            // the SQ ring is currently full and entries must be submitted  for
            // processing before new ones can get allocated.
            auto error = std::make_exception_ptr(
                        std::system_error(EBUSY, std::system_category()));
            stdexec::set_error(std::move(receiver), std::move(error));
        }
    }

    inline constexpr static vtable this_vtable {
        .complete = [](auto *_self, result_t cqe_res) noexcept {
            auto self = static_cast<io_uring_exec_operation*>(_self);

            constexpr auto is_timer = [] {
                // Make GCC happy.
                if constexpr (requires { F == &io_uring_prep_timeout; })
                    return F == &io_uring_prep_timeout;
                return false;
            } ();

            // Zero overhead for regular operations.
            if constexpr (is_timer) {
                auto good = [cqe_res](auto ...errors) { return ((cqe_res == errors) || ...); };
                // Timed out is not an error.
                if(good(-ETIME, -ETIMEDOUT)) [[likely]] {
                    stdexec::set_value(std::move(self->receiver), cqe_res);
                    return;
                }
            }

            if(cqe_res >= 0) [[likely]] {
                stdexec::set_value(std::move(self->receiver), cqe_res);
            } else if(cqe_res == -ECANCELED) {
                stdexec::set_stopped(std::move(self->receiver));
            } else {
                auto error = std::make_exception_ptr(
                            std::system_error(-cqe_res, std::system_category()));
                stdexec::set_error(std::move(self->receiver), std::move(error));
            }
        }
    };

    Receiver receiver;
    io_uring_exec *uring;
    std::tuple<Args...> args;
};

using io_uring_exec_operation_base = io_uring_exec::io_uring_exec_operation_base;

template <auto io_uring_prep_invocable, typename ...Args>
struct io_uring_exec_sender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
                                    stdexec::set_value_t(io_uring_exec_operation_base::result_t),
                                    stdexec::set_error_t(std::exception_ptr),
                                    stdexec::set_stopped_t()>;

    template <stdexec::receiver Receiver>
    io_uring_exec_operation<io_uring_prep_invocable, Receiver, Args...>
    connect(Receiver receiver) noexcept {
        return {std::move(receiver), uring, std::move(args)};
    }

    io_uring_exec *uring;
    std::tuple<Args...> args;
};

template <auto io_uring_prep_invocable, typename ...Args>
stdexec::sender_of<
    stdexec::set_value_t(io_uring_exec_operation_base::result_t /* cqe->res */),
    stdexec::set_error_t(std::exception_ptr)>
auto make_uring_sender(std::in_place_t,
                       io_uring_exec::scheduler s, std::tuple<Args...> &&t_args) noexcept {
    return io_uring_exec_sender<io_uring_prep_invocable, Args...>{s.uring, std::move(t_args)};
}

// A sender factory.
template <auto io_uring_prep_invocable>
auto make_uring_sender(io_uring_exec::scheduler s, auto &&...args) noexcept {
    return make_uring_sender<io_uring_prep_invocable>
        (std::in_place, s, std::tuple(static_cast<decltype(args)&&>(args)...));
}

// On  files  that  support seeking, if the `offset` is set to -1, the read operation commences at the file offset,
// and the file offset is incremented by the number of bytes read. See read(2) for more details. Note that for an
// async API, reading and updating the current file offset may result in unpredictable behavior, unless access to
// the file is serialized. It is **not encouraged** to use this feature, if it's possible to provide the  desired  IO
// offset from the application or library.
stdexec::sender
auto async_read(io_uring_exec::scheduler s, int fd, void *buf, size_t n, uint64_t offset = 0) noexcept {
    return make_uring_sender<io_uring_prep_read>(s, fd, buf, n, offset);
}

stdexec::sender
auto async_write(io_uring_exec::scheduler s, int fd, const void *buf, size_t n, uint64_t offset = 0) noexcept {
    return make_uring_sender<io_uring_prep_write>(s, fd, buf, n, offset);
}

stdexec::sender
auto async_wait(io_uring_exec::scheduler s, std::chrono::milliseconds duration) noexcept {
    using namespace std::chrono;
    auto duration_s = duration_cast<seconds>(duration);
    auto duration_ns = duration_cast<nanoseconds>(duration - duration_s);
    return
        // `ts` needs safe lifetime within an asynchronous scope.
        stdexec::just(__kernel_timespec {
            .tv_sec = duration_s.count(),
            .tv_nsec = duration_ns.count()
        })
      | stdexec::let_value([s](auto &&ts) {
            return make_uring_sender<io_uring_prep_timeout>(std::in_place, s,
                []<typename R, typename ...Ts>(R(io_uring_sqe*, Ts...), auto &&...args) {
                    return std::tuple<Ts...>{static_cast<decltype(args)&&>(args)...};
                }(io_uring_prep_timeout, &ts, 0, 0));
        });
}
