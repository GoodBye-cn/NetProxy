#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "Socket.h"

namespace netproxy::net {

/**
 * 面向连接的 TCP 套接字。
 *
 * 创建与绑定由基类按 SOCK_STREAM / IPPROTO_TCP 完成，
 * 本类只提供连接、监听、收发等 TCP 语义接口。
 */
class TCPSocket final : public Socket {
public:
    // 构造 TCP 套接字，句柄在 open 或 bind 时创建。
    TCPSocket() : Socket(SOCK_STREAM, IPPROTO_TCP) {}

    /**
     * 连接到目标地址。
     *
     * 未创建句柄时会先按目标地址族创建；失败不关闭句柄，由调用方决定是否 close()。
     *
     * @param address 目标地址，包含 IP 与端口。
     * @return 连接成功返回 true；地址无效或系统调用失败返回 false，并已记录日志。
     */
    bool connect(const IPAddress& address);

    /**
     * 开始监听，需先绑定地址。
     *
     * @param backlog 等待 accept 的连接队列长度，默认 16。
     * @return 成功返回 true；句柄无效或系统调用失败返回 false。
     */
    bool listen(int backlog = 16);

    /**
     * 接受一个已建立的连接。
     *
     * @return 新的 TCP 套接字；失败时返回 isValid() 为 false 的对象，并已记录日志。
     */
    TCPSocket accept();

    /**
     * 发送数据。
     *
     * @param data 待发送缓冲区首地址。
     * @param size 待发送字节数。
     * @return 实际发送的字节数；失败返回 -1，并已记录日志。
     */
    int send(const void* data, std::size_t size);

    /**
     * 发送字符串。
     *
     * @param data 待发送内容。
     * @return 实际发送的字节数；失败返回 -1。
     */
    int send(const std::string& data);

    /**
     * 接收数据。
     *
     * @param buffer 接收缓冲区首地址。
     * @param size 缓冲区容量。
     * @return 实际接收的字节数；对端正常关闭返回 0；失败返回 -1 并已记录日志。
     */
    int receive(void* buffer, std::size_t size);

    /**
     * 接收数据并以字符串返回。
     *
     * @param maxSize 单次最多接收的字节数。
     * @return 收到的内容；无数据、对端关闭或失败时返回空串。
     */
    std::string receive(std::size_t maxSize);

    /**
     * 关闭发送方向。
     *
     * 对端随后会读到 0，本地仍可继续接收。
     *
     * @return 成功返回 true；句柄无效或系统调用失败返回 false。
     */
    bool shutdownWrite();

    /**
     * 开启或关闭 Nagle 算法。
     *
     * @param enabled true 关闭 Nagle（低延迟），false 开启 Nagle（合并小包）。
     * @return 设置成功返回 true；句柄无效或系统调用失败返回 false。
     */
    bool setNoDelay(bool enabled);

private:
    // 接管 accept 返回的句柄。
    explicit TCPSocket(SocketHandle handle) : Socket(handle, SOCK_STREAM, IPPROTO_TCP) {}
};

} // namespace netproxy::net
