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

template <typename Mutex = std::mutex,
          size_t N_way_concurrency = 4>
struct multi_lock {
    auto fastpath_guard(const std::derived_from<immovable> auto &stable_object) {
        using T = std::decay_t<decltype(stable_object)>;
        auto to_value = std::bit_cast<std::uintptr_t, const T*>;
        auto no_align = [&](auto v) { return v / alignof(T); };
        auto to_index = [&](auto v) { return v % N_way_concurrency; };
        auto then = std::views::transform;
        auto view = std::views::single(&stable_object)
                  | then(to_value)
                  | then(no_align)
                  | then(to_index);
        return std::unique_lock{_mutexes[view[0]]};
    }

    auto slowpath_guard() {
        auto make_scoped_lock = [](auto &&...mutexes) {
            return std::scoped_lock{mutexes...};
        };
        return std::apply(make_scoped_lock, _mutexes);
    }

    std::array<Mutex, N_way_concurrency> _mutexes;
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

struct you_are_a_vtable_signature {};

template <typename Signature>
concept vtable_signature = requires {
    typename Signature::sign_off;
    requires std::is_same_v<typename Signature::sign_off,
                            you_are_a_vtable_signature>;
};

// Make vtable composable.
template <typename>
struct add_complete_to_vtable;

// Make vtable composable.
template <typename>
struct add_cancel_to_vtable;

template <typename Ret, typename ...Args>
struct add_complete_to_vtable<Ret(Args...)> {
    using sign_off = you_are_a_vtable_signature;
    Ret (*complete)(Args...);
};

template <typename Ret, typename ...Args>
struct add_cancel_to_vtable<Ret(Args...)> {
    using sign_off = you_are_a_vtable_signature;
    Ret (*cancel)(Args...);
};

// Per-object vtable for (receiver) type erasure.
// Example:
// using vtable = make_vtable<add_cancel_to_vtable<void()>,
//                            add_complete_to_vtable<int(std::string)>>;
template <vtable_signature ...Signatures>
struct make_vtable: Signatures... {};

} // namespace detail
