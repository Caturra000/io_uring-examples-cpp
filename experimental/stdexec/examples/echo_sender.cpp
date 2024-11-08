#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <ranges>
#include <exec/repeat_effect_until.hpp>
#include "uring_exec.hpp"
#include "utils.h"

// TODO: request stop.
stdexec::sender
auto echo(io_uring_exec::scheduler scheduler, int client_fd) {
    return
        stdexec::schedule(scheduler)
      | stdexec::let_value([] {
            return stdexec::just(std::array<char, 512>{});
         })
      | stdexec::let_value([=](auto &buf) {
            return
                async_read(scheduler, client_fd, buf.data(), buf.size())
              | stdexec::then([=, &buf](int read_bytes) {
                    auto to_console = std::ostream_iterator<char>{std::cout};
                    std::ranges::copy(buf | std::views::take(read_bytes), to_console);
                    return read_bytes;
                })
              | stdexec::let_value([=, &buf](int read_bytes) {
                    return async_write(scheduler, client_fd, buf.data(), read_bytes);
                });
        })
      | stdexec::let_value([](int written_bytes) {
            (void)written_bytes;
            return stdexec::just(false);
        })
      | exec::repeat_effect_until();
}

stdexec::sender
auto server(io_uring_exec::scheduler scheduler, int server_fd, exec::async_scope &scope) {
    return
        stdexec::schedule(scheduler)
      | stdexec::let_value([=] {
            return async_accept(scheduler, server_fd, {});
        })
      | stdexec::let_value([=, &scope](int client_fd) {
            scope.spawn(echo(scheduler, client_fd));
            return stdexec::just(false);
        })
      | exec::repeat_effect_until();
}

int main() {
    auto server_fd = make_server({.port=8848});
    auto server_fd_cleanup = defer([&](...) { close(server_fd); });

    io_uring_exec uring({.uring_entries=512});
    exec::async_scope scope;

    scope.spawn(
        server(uring.get_scheduler(), server_fd, scope));

    uring.run();
}
