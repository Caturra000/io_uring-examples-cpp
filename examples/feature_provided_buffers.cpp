#include <unistd.h>
#include <netinet/in.h>
#include <liburing.h>
#include <utility>
#include <iostream>
#include <algorithm>
#include <array>
#include <ranges>
#include <iterator>
#include "utils.h"
#include "coroutine.h"
#include "feature_provided_buffers.h"

Task echo(io_uring *uring, int client_fd, use_provided_buffers_t provided_buffers_token, auto buffer_helpers) {
    const auto &[buffer_finder, buffer_size, buf_ring_capacity] = buffer_helpers;
    for(;;) {
        // We don’t need to prepare a buffer before completing the operation.
        auto [n, bid] = co_await async_read(provided_buffers_token, client_fd, nullptr, buffer_size);
        n | nofail("read");
        // TODO: fallback to async_read(uring) if ...;

        auto rejoin = defer([&](...) {
            buffer_rejoin(provided_buffers_token, buffer_helpers, bid);
        });

        const auto buf = buffer_finder(bid);
        auto printer = std::ostream_iterator<char>{std::cout};
        std::ranges::copy_n(buf, n, printer);

        n = co_await async_write(uring, client_fd, buf, n) | nofail("write");

        bool close_proactive = n > 2 && buf[0] == 'Z' && buf[1] == 'z';
        bool close_reactive = (n == 0);
        if(close_reactive || close_proactive) {
            co_await async_close(uring, client_fd);
            break;
        }
    }
}

Task server(io_uring *uring, Io_context &io_context, int server_fd, use_provided_buffers_t provided_buffers_token, auto buffer_helpers) {
    for(;;) {
        auto client_fd = co_await async_accept(uring, server_fd) | nofail("accept");
        // Fork a new connection.
        co_spawn(io_context, echo(uring, client_fd, provided_buffers_token, buffer_helpers));
    }
}

int main() {
    auto server_fd = make_server({.port=8848});
    auto server_fd_cleanup = defer([&](...) { close(server_fd); });

    io_uring uring;
    constexpr size_t ENTRIES = 256;
    io_uring_queue_init(ENTRIES, &uring, 0);
    auto uring_cleanup = defer([&](...) { io_uring_queue_exit(&uring); });

    constexpr int BGID = 0;
    constexpr unsigned int RING_ENTRIES = 64;
    constexpr size_t buffer_size = 4096;
    auto [buf_ring, register_buffers, buffer_finder, buf_ring_cleanup]
        = make_provided_buffers(&uring, BGID, RING_ENTRIES, buffer_size);

    Io_context io_context{uring};
    co_spawn(io_context, server(&uring, io_context, server_fd,
            use_provided_buffers(&uring, buf_ring, BGID),
            make_buffer_helper(buffer_finder, buffer_size, RING_ENTRIES)));
    io_context.run();
}
