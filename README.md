# io_uring示例

## 介绍

本仓库提供简单的`io_uring` / `liburing`使用示例，包括同步用法和C++20协程的异步用法。

其中协程封装在`include/coroutine.h`，仅需200行代码。

以下是一个类似Asio C++20协程的echo程序：

```cpp
Task echo(io_uring *uring, int client_fd) {
    char buf[4096];
    for(;;) {
        auto n = co_await async_read(uring, client_fd, buf, std::size(buf)) | nofail("read");

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

Task server(io_uring *uring, Io_context &io_context, int server_fd) {
    for(;;) {
        auto client_fd = co_await async_accept(uring, server_fd) | nofail("accept");
        // Fork a new connection.
        co_spawn(io_context, echo(uring, client_fd));
    }
}

int main() {
    auto server_fd = make_server(8848);
    auto server_fd_cleanup = defer([&](...) { close(server_fd); });

    io_uring uring;
    constexpr size_t ENTRIES = 256;
    io_uring_queue_init(ENTRIES, &uring, 0);
    auto uring_cleanup = defer([&](...) { io_uring_queue_exit(&uring); });

    Io_context io_context{uring};
    co_spawn(io_context, server(&uring, io_context, server_fd));
    io_context.run();
}
```

其它示例请看`examples`目录：
1. `cat.cpp`：类似`cat`命令。
2. `echo.cpp`：使用回调的echo程序。
3. `echo_coroutine.cpp`：使用协程的echo程序。
4. `test_multi_task.cpp`：`co_await Task`测试。
5. `feature_multishot.cpp`：使用multishot特性的监听服务器。
6. `feature_multishot2.cpp`：使用multishot与协程的echo程序。
7. `feature_sqpoll.cpp`：使用SQPOLL特性的echo程序，更多细节见`feature_sqpoll.h`文件。
8. `feature_io_drain.cpp`：使用IO drain特性的echo程序。
9. `feature_io_link.cpp`：使用IO link特性的单向回复程序。
10. `feature_provided_buffers.cpp`：使用provided buffers特性的echo程序。
11. `test_context_switch.cpp`：`co_await switch_to(io_context)`跨线程上下文切换测试。

## 构建

构建使用`make all`命令，可执行文件会生成于`build`目录。

NOTE: 请确保编译器支持C++20标准，以及系统已安装`liburing`，内核版本过低（推荐6.1+）同样无法编译。


## 其它示例

具体看代码文件，这里只做节选。

```cpp
// Multishot, 只对内核启动一个异步操作，但是允许多次完成
Task server(io_uring *uring, Io_context &io_context, int server_fd) {
    // A multishot (submit once) awaiter.
    auto awaiter = async_multishot_accept(uring, server_fd);
    for(;;) {
        // No need to submit more sqes.
        auto client_fd = co_await awaiter | nofail("multishot_accept");
        co_spawn(io_context, echo(uring, client_fd));
    }
}
```

```cpp
// IO link，使用&或者|来链式同步操作，无需多次co_await
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
```

```cpp
// IO drain，完成通知前需先完成此前启动的异步操作
Task server(io_uring *uring, Io_context &io_context, int server_fd) {
    for(;;) {
        auto client_fd = co_await async_drain_accept(uring, server_fd) | nofail("accept");
        co_spawn(io_context, echo(uring, client_fd));
    }
}
```

```cpp
// 通过co_await switch_to()来回切换不同的io_context上下文
Task just_print(io_uring (&uring)[2], Io_context (&io_context)[2]) {
    for(size_t i = 0; ; i ^= 1) {
        std::ostringstream oss;
        oss << "current thread is "
            << std::this_thread::get_id()
            << std::endl;
        auto string = oss.str();
        co_await async_write(&uring[i], 1, string.data(), string.size());

        // Switch to the context where io_context[i^1] is running.
        co_await switch_to(io_context[i ^ 1]);
    }
}
```

```cpp
// provided buffers，无需在异步操作前预备buffer
Task echo(io_uring *uring, int client_fd, auto provided_buffers_token, auto buffer_helpers) {
    const auto &[buffer_finder, buffer_size, _] = buffer_helpers;
    for(;;) {
        // We don’t need to prepare a buffer before completing the operation.
        auto [n, bid] = co_await async_read(provided_buffers_token, client_fd, nullptr, buffer_size);

        auto rejoin = defer([&](...) {
            buffer_rejoin(provided_buffers_token, buffer_helpers, bid);
        });

        const auto buf = buffer_finder(bid);
        auto printer = std::ostream_iterator<char>{std::cout};
        std::ranges::copy_n(buf, n, printer);
        co_await async_write(uring, client_fd, buf, n) | nofail("write");
        // ...
    }
}
```

TODO: 高级特性待补充。
