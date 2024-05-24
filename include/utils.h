#pragma once
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <type_traits>
#include <bit>
#include <array>
#include <atomic>

// C-style check for syscall.
inline void check(int cond, const char *reason) {
    if(!cond) [[likely]] return;
    perror(reason);
    abort();
}

// C++-style check for syscall.
// Failed on ret < 0 by default.
//
// INT_ASYNC_CHECK: helper for liburing (-ERRNO) and other syscalls (-1).
// It may break generic programming (forced to int).
template <typename Comp = std::less<int>, auto V = 0, bool INT_ASYNC_CHECK = true>
struct nofail {
    std::string_view reason;

    // Examples:
    // fstat(...) | nofail("fstat");        // Forget the if-statement and ret!
    // int fd = open(...) | nofail("open"); // If actually need a ret, here you are!
    friend decltype(auto) operator|(auto &&ret, nofail nf) {
        if(Comp{}(ret, V)) [[unlikely]] {
            // Hack errno.
            if constexpr (INT_ASYNC_CHECK) {
                using T = std::decay_t<decltype(ret)>;
                static_assert(std::is_convertible_v<T, int>);
                // -ERRNO
                if(ret != -1) errno = -ret;
            }
            perror(nf.reason.data());
            std::terminate();
        }
        return std::forward<decltype(ret)>(ret);
    };
};

// Make clang happy.
nofail(...) -> nofail<std::less<int>, 0, true>;

// Go-style, move-safe defer.
[[nodiscard("defer() is not allowed to be temporary.")]]
inline auto defer(auto func) {
    // Make STL happy.
    auto dummy = reinterpret_cast<void*>(0x1);
    return std::unique_ptr<void, decltype(func)>{dummy, std::move(func)};
}

// For make_server().
struct make_server_option_t {
    int port {8848};
    int backlog {128};
    bool nonblock {false};
    bool reuseaddr {true};
    bool reuseport {false};
};

// Do some boring stuff and return a server fd.
inline int make_server(make_server_option_t option) {
    int socket_flag = option.nonblock ? SOCK_NONBLOCK : 0;
    int socket_fd = socket(AF_INET, SOCK_STREAM, socket_flag) | nofail("socket");

    auto setsock = [enable = 1, fd = socket_fd](int optname) {
        setsockopt(fd, SOL_SOCKET, optname, &enable, sizeof(int)) | nofail("setsockopt");
    };
    if(option.reuseaddr) setsock(SO_REUSEADDR);
    if(option.reuseport) setsock(SO_REUSEPORT);

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(option.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // About the strict aliasing rule:
    // `reinterpret_cast` is OK if we don't derenference it (with the wrong type).
    // But what about the implementation of `bind()`?
    //   - `bind()` must dereference the object with a wrong type.
    // Can we memcpy an actual `sockaddr` from `sockaddr_xxx` then pass it to `bind()`?
    //   - Normally, memcpy is a good way to make type punning safe.
    //   - It is guaranteed by the C (and C++) standard to access through an object.
    //   - But answer is NO, because `sizeof(sockaddr_xxx) <= sizeof(sockaddr)` is not necessarily true.
    //   - How can you access an object without proper size and alignment?
    //   - `std::launder()` with cast / `std::start_lifetime_as()` cannot solve the `sizeof()` problem either.
    // Does it violate the strict aliasing rule?
    //   - Maybe. It depends on the library side. We cant do any more.
    //   - But many people explicitly cast types with UNIX interfaces.
    //   - And compilers should not offend users, especially for legacy codes.
    //   - So practically, it is OK.
    auto no_alias_addr = reinterpret_cast<const sockaddr*>(&addr);

    bind(socket_fd, no_alias_addr, sizeof(addr)) | nofail("bind");

    listen(socket_fd, option.backlog) | nofail("listen");

    return socket_fd;
}

// A lock-free, single-producer-single-consumer and fixed-size FIFO implementation.
// (Mostly copied from linux/kfifo.)
// That is:
// Only one thread may call push().
// Only one thread may call pop().
//
// NOTE:
// For multiple writer and one reader there is only a need to lock the writer.
// For only one writer and multiple reader there is only a need to lock the reader.
//
// T: type of elements.
// SIZE: size of the allocated buffer. MUST be a power of 2.
template <typename /*TODO: concept*/ T, size_t SIZE>
struct Fifo {
    std::array<T, SIZE> _buffer;
    alignas(64) std::atomic<size_t> _in {};
    alignas(64) std::atomic<size_t> _out {};

    constexpr Fifo() noexcept { static_assert(SIZE > 1 && !(SIZE & (SIZE-1)), "Read the comments!"); }

    // For mod computation.
    inline constexpr static size_t MASK = SIZE - 1;

    bool push(T elem) noexcept {
        // Only producer can modify _in.
        size_t in = _in.load(std::memory_order_relaxed);
        size_t next_in = (in+1) & MASK;
        if(next_in == _out.load(std::memory_order_acquire)) {
            return false;
        }
        _buffer[in] = elem;
        // Let consumer know your change.
        _in.store(next_in, std::memory_order_release);
        return true;
    }

    void pop(T &opt) noexcept {
        size_t in = _in.load(std::memory_order_acquire);
        size_t out = _out.load(std::memory_order_relaxed);
        if(in == out) return;
        opt = _buffer[out];
        _out.store((out+1) & MASK, std::memory_order_release);
    }
};
