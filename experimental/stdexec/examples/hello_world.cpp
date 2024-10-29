#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <ranges>
#include "uring_exec.hpp"

int main() {
    constexpr auto test_file_name = "/tmp/jojo";
    int fd = (::unlink(test_file_name), ::open(test_file_name, O_RDWR|O_TRUNC|O_CREAT, 0666));
    if(fd < 0 || ::write(fd, "dio", 3) != 3) {
        std::cerr << ::strerror(errno) , std::abort();
    }

    io_uring_exec uring(512);
    auto scheduler = uring.get_scheduler();
    exec::async_scope scope;
    auto s1 =
        stdexec::schedule(scheduler)
      | stdexec::then([] {
            std::cout << "hello ";
            return 19260816;
        })
      | stdexec::let_value([](int v) {
            return stdexec::just(v+1);
        }); // or then(...)

    auto s2 =
        stdexec::schedule(scheduler)
      | stdexec::then([] {
            std::cout << "world!" << std::endl;
            return std::array<char, 5>{};
        })
      | stdexec::let_value([=](auto &buf) {
            return async_read(scheduler, fd, buf.data(), buf.size())
              | stdexec::then([&](auto nread) {
                    auto bview = buf | std::views::take(nread);
                    for(auto b : bview) std::cout << b;
                    std::cout << std::endl;
                    return nread;
                });
        });

    std::jthread j {[&] { uring.run(); }};

    // scope.spawn(std::move(s1) | stdexec::then([](...) {}));
    // scope.spawn(std::move(s2) | stdexec::then([](...) {}));
    // stdexec::sync_wait(scope.on_empty());

    auto a =
        stdexec::when_all(std::move(s1), std::move(s2));
    auto [v1, v2] = stdexec::sync_wait(std::move(a)).value();
    ::unlink(test_file_name);
    std::cout << v1 << ' ' << v2 << std::endl;
}
