#pragma once
#include <liburing.h>
#include <atomic>
#include "coroutine.h"

struct context_switch_operation {
    // Must suspend in the current context!
    constexpr bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) noexcept {
        bool _ok = io_context.push_to_switch(h);
        ok.store(_ok, std::memory_order_release);
        return _ok;
    }
    bool await_resume() const noexcept { return ok.load(std::memory_order_acquire); }

    context_switch_operation(Io_context &io_context) noexcept: io_context(io_context) {}

    Io_context &io_context;
    std::atomic<bool> ok;
};

inline auto switch_to(Io_context &next_io_context) noexcept {
    return context_switch_operation(next_io_context);
}
