#include <iostream>
#include "uring_exec.hpp"

int main() {
    // Default behavior: Infinite run().
    // {
    //     io_uring_exec uring({.uring_entries=8});
    //     std::jthread j([&] { uring.run(); });
    // }

    // Per-run() user-defined stop source (token).
    {
        io_uring_exec uring({.uring_entries=8});
        std::stop_source external_stop_source;
        std::jthread j(&io_uring_exec::run<>, &uring, external_stop_source.get_token());
        external_stop_source.request_stop();
    }
    std::cout << "case 1: stopped." << std::endl;

    // Per-io_uring_exec stop source.
    {
        io_uring_exec uring({.uring_entries=8});
        std::jthread j([&] { uring.run(); });
        uring.request_stop();
    }
    std::cout << "case 2: stopped." << std::endl;

    // std::jthread stop source.
    {
        io_uring_exec uring({.uring_entries=8});
        std::jthread j([&](std::stop_token token) { uring.run(token); });
    }
    std::cout << "case 3: stopped." << std::endl;

    // Heuristic Algorithm (autoquit).
    {
        io_uring_exec uring({.uring_entries=8});
        constexpr auto autoquit_policy = io_uring_exec::run_policy {.autoquit=true};
        std::jthread j([&] { uring.run<autoquit_policy>(); });
    }
    std::cout << "case 4: stopped." << std::endl;
}
