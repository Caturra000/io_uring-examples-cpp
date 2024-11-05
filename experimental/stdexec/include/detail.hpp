#pragma once
#include <mutex>
#include <array>
#include <tuple>
#include <concepts>
#include <ranges>

namespace detail {

// Operations in stdexec are required to be address-stable.
struct immovable {
    immovable() = default;
    immovable(immovable &&) = delete;
};

struct submit_lock {
    auto fastpath_guard(std::derived_from<immovable> auto &stable_object) {
        using T = std::decay_t<decltype(stable_object)>;
        auto to_value = std::bit_cast<std::uintptr_t, T*>;
        auto no_align = [&](auto v) { return v / alignof(T); };
        auto to_index = [&](auto v) { return v % n_way_concurrency; };
        auto then = std::views::transform;
        auto view = std::views::single(&stable_object)
                  | then(to_value)
                  | then(no_align)
                  | then(to_index);
        return std::unique_lock{_submit_mutexes[view.begin()[0]]};
    }

    auto slowpath_guard() {
        return std::apply([](auto &&...mutexes) {
                            return std::scoped_lock{mutexes...};
                          },
                          _submit_mutexes);
    }

    inline static constexpr size_t n_way_concurrency = 4;
    std::array<std::mutex, n_way_concurrency> _submit_mutexes;
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
