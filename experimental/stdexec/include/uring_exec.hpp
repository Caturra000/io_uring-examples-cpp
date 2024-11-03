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
        std::atomic<bool> seen {false};
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
        bool autoquit {false};          // `concurrent` runs infinitely by default.
        bool lockless {false};          // (WIP) Some intrusive queues are within the same thread.
        bool realtime {false};          // No deferred processing.
        bool waitable {false};          // Submit and wait.
        bool hookable {true};           // Always true beacause of per-object vtable.
    };

    // Run with customizable policy.
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
    template <run_policy policy = {}>
    void run() {
        // Progress.
        task *first_task {};        // For empty queue detection.
        ssize_t submitted {};       // For `io_uring_submit`.
        ssize_t done {};            // For `io_uring_for_each_cqe`.

        // Deferred processing.
        bool deferred_initialization {};
        ssize_t local_inflight {};
        ssize_t plugged_done {};
        constexpr size_t scheduled_sync_ratio = 32;

        auto unplug = [&](...) {
            if constexpr (policy.realtime) return;
            if(plugged_done == 0) return;
            _inflight.fetch_sub(plugged_done);
        };
        auto make_STL_happy = reinterpret_cast<void*>(0x1);
        auto unplug_on_exit = std::unique_ptr<void, decltype(unplug)>
                                {make_STL_happy, std::move(unplug)};

        // TODO: stop_token.
        for(auto step : std::views::iota(1 /* Avoid pulling immediately */)) {
            if constexpr (policy.launch) {
                // TODO: Optimzied to a lockfree version?
                // But the move operation is just trying to lock once...
                auto [first, last] = move_task_queue();
                if(first) for(auto op = first;; op = op->next) {
                    op->vtab.complete(op);
                    if(op == last) break;
                }
                first_task = first;
            }

            // `_inflight` is only needed when `autoquit` is enabled.
            // We can do deferred processing on it to avoid any communication overhead.
    
            if constexpr (policy.submit) {
                // Must hold a lock. There are many data races
                // between sqe allocation/preparation and submission.
                std::lock_guard guard {_submit_mutex};
                // TODO: wait_{one|some|all}.
                if constexpr (policy.waitable) {
                    submitted = io_uring_submit_and_wait(&_underlying_uring, 1);
                } else {
                    submitted = io_uring_submit(&_underlying_uring);
                }
            }

            if constexpr (policy.iodone) {
                io_uring_cqe *cqe;
                done = 0;
                constexpr auto mo = std::memory_order::relaxed;

                // There may be a contention between concurrent run()s.
                while(!io_uring_peek_cqe(&_underlying_uring, &cqe)) {
                    using uop = io_uring_exec_operation_base;
                    auto uring_op = std::bit_cast<uop*>(cqe->user_data);
                    // It won't change any other shared variable,
                    // and reorder is ok because of data/control dependency,
                    // so just use relaxed order.
                    bool expected = false, desired = true;
                    // NOTES:
                    // * Not allowed to fail spuriously, use strong version.
                    // * xchg should be more API-friendly than cmpxchg,
                    //   but we need to consider performance on read-side failure.
                    if(auto &seen = uring_op->seen;
                       !seen.compare_exchange_strong(expected, desired, mo, mo))
                    {
                        continue;
                    }
                    // Cached before seen(), io_uring may overwrite this value.
                    auto cqe_res = cqe->res;
                    io_uring_cqe_seen(&_underlying_uring, cqe);
                    done++;
                    uring_op->vtab.complete(uring_op, cqe_res);
                }
            }

            if constexpr (policy.realtime) {
                // Avoid 0.
                if(done) {
                    constexpr auto acq_rel = std::memory_order::acq_rel;
                    local_inflight = _inflight.fetch_sub(done, acq_rel);
                }
            } else if constexpr (policy.autoquit) {
                plugged_done += done;
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

            if constexpr (not policy.busyloop) {
                if(any_progress) std::this_thread::yield();
            }

            if constexpr (policy.autoquit) {
                if(any_progress) continue;
                bool scheduled_synchronizable = (step % scheduled_sync_ratio == 0);
                constexpr struct {
                    struct {} forced;
                    struct {} normal;
                } mode;
                auto synchronize = [&, synchronized = false](auto tag = {}) mutable {
                    constexpr bool forced = requires { tag == mode.forced; };
                    if constexpr (!forced) {
                        if(std::exchange(synchronized, true)) return;
                    }
                    constexpr auto mo = forced ?
                          std::memory_order::acq_rel
                        : std::memory_order::relaxed;
                    if(auto delta = std::exchange(plugged_done, 0)) {
                        local_inflight = _inflight.fetch_sub(delta, mo);
                    } else {
                        local_inflight = _inflight.load(mo);
                    }
                };

                // 1. On-demand synchronization.
                if(!deferred_initialization) {
                    deferred_initialization = true;
                    synchronize(mode.normal);
                }
                // 2. Scheduled synchronization.
                if(scheduled_synchronizable) {
                    synchronize(mode.normal);
                }
                // 3. Best-effort synchronization before actual `return`.
                if(local_inflight <= 0) {
                    synchronize(mode.forced);
                }
                // 4. Finally...
                if(local_inflight <= 0) {
                    return;
                }
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
    // The `_inflight` value is estimated (or inaccurate).
    std::atomic<ssize_t> /*_estimated*/_inflight {};

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
