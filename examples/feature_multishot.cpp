#include <liburing.h>
#include <iostream>
#include <cassert>
#include <vector>
#include "utils.h"

// In this program, server does nothing but accept new connections.
// It will accept exactly 3 clients, then exit normally.
int main() {
    auto server_fd = make_server(8848);
    auto server_fd_cleanup = defer([&](...) { close(server_fd); });

    io_uring uring;
    io_uring_queue_init(256, &uring, 0);
    auto uring_cleanup = defer([&](...) { io_uring_queue_exit(&uring); });

    auto sqe = io_uring_get_sqe(&uring);
    // Feature: multishot accept.
    io_uring_prep_multishot_accept(sqe, server_fd, nullptr, nullptr, 0);

    // Submit ONCE!
    int n = io_uring_submit(&uring);
    assert(n == 1);

    std::vector<int> client_fds;

    while(client_fds.size() < 3) {
        io_uring_cqe *cqe;

        io_uring_wait_cqe(&uring, &cqe) | nofail("wait");

        // IORING_CQE_F_MORE:
        // If set, parent SQE will generate more CQE entries.
        if(!(cqe->flags & IORING_CQE_F_MORE)) {
            std::cerr << "[Warning] Multishot disabled." << std::endl;
        }

        int client_fd = cqe->res | nofail("accept");
        client_fds.emplace_back(client_fd);
        std::cout << "New client accepted! fd: " << client_fd << std::endl;
        io_uring_cqe_seen(&uring, cqe);
    }

    for(auto fd : client_fds) close(fd);
}
