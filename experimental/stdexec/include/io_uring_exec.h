#pragma once
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>
#include <tuple>
#include <liburing.h>
#include <stdexec/execution.hpp>
#include <exec/async_scope.hpp>
#include "detail.hpp"
#include "io_uring_exec_run.h"

using immovable = detail::immovable;

struct io_uring_exec: immovable, io_uring_exec_run<io_uring_exec> {
    io_uring_exec(size_t uring_entries, int uring_flags = 0): _intrusive_queue{._head{{}, {}}} {
        if(int err = io_uring_queue_init(uring_entries, &_underlying_uring, uring_flags)) {
            throw std::system_error(-err, std::system_category());
        }
    }

    ~io_uring_exec() {
        io_uring_queue_exit(&_underlying_uring);
    }

    // All the tasks are asynchronous.
    // The `task` struct is queued by a user-space intrusive queue.
    // NOTE: The io_uring-specified task is queued by an interal ring of io_uring.
    struct task: immovable {
        using vtable = detail::make_vtable<void(task*)>;
        vtable vtab;
        task *next {nullptr};
    };

    using intrusive_task_queue = detail::intrusive_queue<task>;

    // Required by stdexec.
    template <stdexec::receiver Receiver>
    struct operation: task {
        using operation_state_concept = stdexec::operation_state_t;

        void start() noexcept {
            uring->_intrusive_queue.push(this);
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
        using vtable = detail::make_vtable<void(io_uring_exec_operation_base*, result_t)>;
        vtable vtab;
        std::atomic<bool> seen {false};
    };

    // Run with customizable policy.
    //
    // If you want to change a few options based on a default config, try this way:
    // ```
    // constexpr auto policy = [] {
    //     auto policy = io_uring_exec::run_policy{};
    //     policy.launch = false;
    //     policy.realtime = true;
    //     // ...
    //     return policy;
    // } ();
    // uring.run<policy>();
    // ```
    //
    // If you want to change only one option, try this way:
    // ```
    // constexpr io_uring_exec::run_policy policy {.busyloop = true};
    // ```
    // NOTE: Designated initializers cannot be reordered.
    using io_uring_exec_run::run_policy;
    using io_uring_exec_run::run;

    // See the comments on `coroutine.h` and `config.h`.
    // The `_inflight` value is estimated (or inaccurate).
    std::atomic<ssize_t> /*_estimated*/_inflight {};
    intrusive_task_queue _intrusive_queue;
    detail::submit_lock _submit_lock;
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
        auto guard = uring->_submit_lock.fastpath_guard(*this);
        if(auto sqe = io_uring_get_sqe(&uring->_underlying_uring)) [[likely]] {
            io_uring_sqe_set_data(sqe, static_cast<io_uring_exec_operation_base*>(this));
            std::apply(F, std::tuple_cat(std::tuple(sqe), std::move(args)));
            uring->_inflight.fetch_add(1, std::memory_order::relaxed);
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
