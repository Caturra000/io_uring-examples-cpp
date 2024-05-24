#include <liburing.h>
#include <string_view>
#include <iostream>
#include "utils.h"
#include "coroutine.h"
#include "feature_io_link.h"

// Say hello, wave goodbye.
Task sayhello(io_uring *uring, int client_fd) {
    using namespace std::literals;
    auto hello = "hello "sv;
    auto world = "world!\n"sv;
    // Actually co_await only for the last one. (by proxy)
    // But still keep that orders.
    co_await (async_write(uring, client_fd, hello.data(), hello.size())
            & async_write(uring, client_fd, world.data(), world.size())
            & async_close(uring, client_fd));
}

Task server(io_uring *uring, Io_context &io_context, int server_fd) {
    for(;;) {
        auto client_fd = co_await async_accept(uring, server_fd) | nofail("accept");
        // Fork a new connection.
        co_spawn(io_context, sayhello(uring, client_fd));
    }
}

int main() {
    auto server_fd = make_server({.port=8848});
    auto server_fd_cleanup = defer([&](...) { close(server_fd); });

    io_uring uring;
    constexpr size_t ENTRIES = 256;
    io_uring_queue_init(ENTRIES, &uring, 0);
    auto uring_cleanup = defer([&](...) { io_uring_queue_exit(&uring); });

    Io_context io_context{uring};
    co_spawn(io_context, server(&uring, io_context, server_fd));
    io_context.run();
}
