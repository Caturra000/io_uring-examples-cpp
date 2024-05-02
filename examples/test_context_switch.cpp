#include <liburing.h>
#include <thread>
#include <chrono>
#include <iostream>
#include <syncstream>
#include <format>
#include "coroutine.h"
#include "utils.h"
#include "co_context_switch.h"

Task just_print(io_uring (&uring)[2], Io_context (&io_context)[2]) {
    for(size_t i = 0; ; i ^= 1) {
        std::ostringstream oss;
        oss << "current thread is "
            << std::this_thread::get_id()
            << std::endl;
        auto string = oss.str();
        co_await async_write(&uring[i], 1, string.data(), string.size());

        using namespace std::chrono_literals;
        std::this_thread::sleep_for(1s);

        // Switch to the context where io_context[j] is running.
        size_t j = i ^ 1;
        auto ok = co_await switch_to(io_context[j]);
        if(!ok) {
            std::cerr << "Too busy to switch." << std::endl;
            i = j;
        }
    }
}

int main() {
    io_uring uring[2];
    constexpr size_t ENTRIES = 256;
    io_uring_queue_init(ENTRIES, &uring[0], 0);
    io_uring_queue_init(ENTRIES, &uring[1], 0);
    auto uring_cleanup = defer([&](...) {
        io_uring_queue_exit(&uring[0]);
        io_uring_queue_exit(&uring[1]);
    });

    Io_context io_context[2] {
        Io_context(uring[0]),
        Io_context(uring[1])
    };

    std::jthread new_thread([&] {
        std::osyncstream(std::cout) << "new thread: "
                << std::this_thread::get_id() << std::endl;
        io_context[1].run();
    });

    std::osyncstream(std::cout) << "local thread: "
                << std::this_thread::get_id() << std::endl;
    co_spawn(io_context[0], just_print(uring, io_context));
    io_context[0].run();
}
