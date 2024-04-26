# io_uring示例

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
4. `multi_task_test.cpp`：`co_await Task`测试。

构建使用`make all`命令，可执行文件会生成于`build`目录。

NOTE: 请确保编译器支持C++20标准，以及系统已安装`liburing`。

TODO: 高级特性待补充。
