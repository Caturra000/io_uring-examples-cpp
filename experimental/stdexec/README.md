# uring_exec

## 介绍

尝试为`io_uring`提供[`stdexec`](https://github.com/NVIDIA/stdexec)支持。

目前已集成`async_read/async_write`接口，使用方法见`examples`目录。

## 构建

`make all`前需确保本地已包含`stdexec`。
