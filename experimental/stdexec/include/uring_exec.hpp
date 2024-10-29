#pragma once
#include <cstring>
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
        task *next{this};
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
    struct uring_operation: immovable {
        using result_t = decltype(std::declval<io_uring_cqe>().res);
        using vtable = make_vtable<void(uring_operation*, result_t)>;
        vtable vtab;
    };

    // TODO: Concurrent run().
    void run() {
        // TODO: stop_token.
        for(task *first_task;;) {
            for(task *op = first_task = pop(); op; op = pop()) {
                op->vtab.complete(op);
            }

            {
                std::lock_guard _{_mutex};
                // Return value is ignored.
                // See the comments on `coroutine.h`.
                io_uring_submit(&_underlying_uring);
            }

            io_uring_cqe *cqe;
            unsigned head;
            unsigned done = 0;
            // Reap one operation / multiple operations.
            // NOTE: One sqe can generate multiple cqes.
            io_uring_for_each_cqe(&_underlying_uring, head, cqe) {
                done++;
                auto uring_op = std::bit_cast<uring_operation*>(cqe->user_data);
                uring_op->vtab.complete(uring_op, cqe->res);
            }
            if(done) {
                io_uring_cq_advance(&_underlying_uring, done);
            } else if(!first_task) {
                std::this_thread::yield();
            }
        }
    }

    task* pop() noexcept {
        // Read only.
        if(_head.next == &_head) {
            return nullptr;
        }
        std::lock_guard _{_mutex};
        auto popped = std::exchange(_head.next, _head.next -> next);
        if(popped == _tail) _tail = &_head;
        return popped;
    }

    void push(task *op) noexcept {
        std::lock_guard _{_mutex};
        op->next = &_head;
        _tail = _tail->next = op;
    }

    task _head, *_tail{&_head};
    std::mutex _mutex;
    io_uring _underlying_uring;
};

template <auto F, stdexec::receiver Receiver, typename ...Args>
struct io_uring_exec_initiating_operation: io_uring_exec::uring_operation {
    using operation_state_concept = stdexec::operation_state_t;
    io_uring_exec_initiating_operation(Receiver receiver,
                                       io_uring_exec *uring,
                                       std::tuple<Args...> args) noexcept
        : io_uring_exec::uring_operation{{}, this_vtable},
          receiver(std::move(receiver)),
          uring(uring),
          args(std::move(args)) {}

    void start() noexcept {
        if(auto sqe = io_uring_get_sqe(&uring->_underlying_uring)) [[likely]] {
            io_uring_sqe_set_data(sqe, static_cast<io_uring_exec::uring_operation*>(this));
            std::apply(F, std::tuple_cat(std::tuple(sqe), std::move(args)));
        } else {
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
            auto self = static_cast<io_uring_exec_initiating_operation*>(_self);
            if(cqe_res == -ECANCELED) {
                stdexec::set_stopped(std::move(self->receiver));
            } else if(cqe_res < 0) {
                auto error = std::make_exception_ptr(
                            std::system_error(-cqe_res, std::system_category()));
                stdexec::set_error(std::move(self->receiver), std::move(error));
            } else [[likely]] {
                stdexec::set_value(std::move(self->receiver), cqe_res);
            }
        }
    };

    Receiver receiver;
    io_uring_exec *uring;
    std::tuple<Args...> args;
};

template <auto F, typename ...Args>
struct io_uring_exec_sender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
                                    stdexec::set_value_t(io_uring_exec::uring_operation::result_t),
                                    stdexec::set_error_t(std::exception_ptr),
                                    stdexec::set_stopped_t()>;

    template <stdexec::receiver Receiver>
    io_uring_exec_initiating_operation<F, Receiver, Args...>
    connect(Receiver receiver) noexcept {
        return {std::move(receiver), uring, std::move(args)};
    }

    io_uring_exec *uring;
    std::tuple<Args...> args;
};

// A sender factory.
template <auto F, typename ...Args>
stdexec::sender_of<
    stdexec::set_value_t(io_uring_exec::uring_operation::result_t /* cqe->res */),
    stdexec::set_error_t(std::exception_ptr)>
auto make_uring_sender(io_uring_exec::scheduler s, Args ...args) noexcept {
    return io_uring_exec_sender<F, Args...>{s.uring, std::tuple(std::move(args)...)};
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
