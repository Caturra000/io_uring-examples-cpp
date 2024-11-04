#pragma once
#include <mutex>
#include <array>
#include <tuple>
#include <concepts>

namespace detail {

// Operations in stdexec are required to be address-stable.
struct immovable {
    immovable() = default;
    immovable(immovable &&) = delete;
};

struct submit_lock {
    auto fastpath_guard(immovable &stable_obejct) {
        auto by_value = std::bit_cast<std::ptrdiff_t>(&stable_obejct);
        auto n_way_concurrency = by_value % std::size(_submit_mutexes);
        return std::unique_lock{_submit_mutexes[n_way_concurrency]};
    }
    
    auto slowpath_guard() {
        return std::apply([](auto &&...mutexes) {
                            return std::scoped_lock{mutexes...};
                          },
                          _submit_mutexes);
    }

    std::array<std::mutex, 4> _submit_mutexes;
};

template <std::derived_from<immovable> T>
    requires requires(T *t) { t->next; }
struct intrusive_queue {
    void push(T *op) noexcept {
        std::lock_guard _{_tail_mutex};
        std::unique_lock may_own {_head_mutex, std::defer_lock};
        if(_tail == &_head) may_own.lock();
        _tail = _tail->next = op;
    }

    // No pop(), just move.
    std::pair<T*, T*> move_queue() noexcept {
        // Don't use std::scoped_lock;
        // its ordering algorithm is implementation-defined.
        std::lock_guard _1 {_tail_mutex};
        std::lock_guard _2 {_head_mutex};
        auto first = std::exchange(_head.next, nullptr);
        auto last = std::exchange(_tail, &_head);
        return {first, last};
    }

    T _head, *_tail{&_head};
    std::mutex _head_mutex{};
    std::mutex _tail_mutex{};
};


// Per-object vtable for (receiver) type erasure.
template <typename>
struct make_vtable;

template <typename Ret, typename ...Args>
struct make_vtable<Ret(Args...)> {
    Ret (*complete)(Args...);
    // Ret2 (*ready)(Args2...);
};

} // namespace detail
