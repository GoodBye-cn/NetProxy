#pragma once

#include "Socket.h"

namespace netproxy::net {

/**
 * 唤醒通道，用于打断阻塞中的 poll。
 *
 * POSIX 使用 pipe，两端均为非阻塞；Windows 使用一对回环 UDP 套接字，
 * 因为 WSAPoll 只接受 SOCKET，不能直接使用管道句柄。
 * 读端句柄固定放在 poll 描述符数组的首位。
 */
class WakeupChannel final {
public:
    WakeupChannel() = default;

    // 析构时关闭通道。
    ~WakeupChannel();

    WakeupChannel(const WakeupChannel&) = delete;
    WakeupChannel& operator=(const WakeupChannel&) = delete;

    /**
     * 创建唤醒通道。
     *
     * 重复调用会先关闭已有通道再重新创建。
     *
     * @return 创建成功返回 true；系统调用失败返回 false，并已记录日志。
     */
    bool open();

    // 关闭通道，重复调用安全。
    void close();

    /**
     * 判断通道是否可用。
     *
     * @return 读端与写端均已创建返回 true。
     */
    bool isOpen() const;

    /**
     * 获取读端句柄。
     *
     * @return 可放入 poll 监听可读的句柄；未打开时返回 INVALID_SOCKET_HANDLE。
     */
    SocketHandle handle() const;

    // 写入一个字节唤醒等待方；缓冲区满时忽略失败，等待方仍会被唤醒。
    void notify();

    // 读空通道内的残留字节，非阻塞，供 poll 返回后调用。
    void drain();

private:
    SocketHandle readHandle_ = INVALID_SOCKET_HANDLE;  // 读端句柄
    SocketHandle writeHandle_ = INVALID_SOCKET_HANDLE; // 写端句柄
};

} // namespace netproxy::net
