#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "Socket.h"

namespace netproxy::net {

/**
 * 无连接的 UDP 套接字，支持广播与组播。
 *
 * 创建与绑定由基类按 SOCK_DGRAM / IPPROTO_UDP 完成，
 * 本类只提供数据报收发与 UDP 专属选项。
 */
class UDPSocket final : public Socket {
public:
    // 构造 UDP 套接字，句柄在 open 或 bind 时创建。
    UDPSocket() : Socket(SOCK_DGRAM, IPPROTO_UDP) {}

    /**
     * 开启或关闭广播发送（SO_BROADCAST）。
     *
     * @param enabled true 开启广播，false 关闭。
     * @return 设置成功返回 true；句柄无效或系统调用失败返回 false。
     */
    bool setBroadcast(bool enabled);

    /**
     * 加入 IPv4 组播组。
     *
     * 加入前应先绑定端口，否则收不到发往该组的报文。
     *
     * @param group 组播组地址，如 239.255.42.99。
     * @param interfaceAddress 指定网卡地址；为空表示使用默认网卡。
     * @return 加入成功返回 true；套接字无效、地址非法或系统调用失败返回 false。
     */
    bool joinMulticastGroup(const std::string& group, const std::string& interfaceAddress = std::string());

    /**
     * 离开已加入的 IPv4 组播组。
     *
     * @param group 组播组地址。
     * @param interfaceAddress 指定网卡地址；为空表示默认网卡。
     * @return 离开成功返回 true；套接字无效、地址非法或系统调用失败返回 false。
     */
    bool leaveMulticastGroup(const std::string& group, const std::string& interfaceAddress = std::string());

    /**
     * 设置组播报文是否回环到本机。
     *
     * @param enabled true 允许回环，false 禁止。
     * @return 设置成功返回 true；句柄无效或系统调用失败返回 false。
     */
    bool setMulticastLoop(bool enabled);

    /**
     * 设置组播 TTL。
     *
     * @param ttl 取值范围 0-255，1 表示只在本网段传播。
     * @return 设置成功返回 true；取值越界、句柄无效或系统调用失败返回 false。
     */
    bool setMulticastTtl(int ttl);

    /**
     * 发送数据报到指定地址。
     *
     * 未创建句柄时会先按目标地址族创建。
     *
     * @param data 待发送缓冲区首地址。
     * @param size 待发送字节数。
     * @param address 目标地址，包含 IP 与端口。
     * @return 实际发送的字节数；失败返回 -1，并已记录日志。
     */
    int sendTo(const void* data, std::size_t size, const IPAddress& address);

    /**
     * 发送字符串数据报到指定地址。
     *
     * @param data 待发送内容。
     * @param address 目标地址，包含 IP 与端口。
     * @return 实际发送的字节数；失败返回 -1。
     */
    int sendTo(const std::string& data, const IPAddress& address);

    /**
     * 接收一个数据报。
     *
     * @param buffer 接收缓冲区首地址。
     * @param size 缓冲区容量。
     * @param from 输出发送方地址，包含 IP 与端口。
     * @return 实际接收的字节数；非阻塞无数据或出错时返回 -1。
     */
    int receiveFrom(void* buffer, std::size_t size, IPAddress& from);

private:
    /**
     * 加入或离开组播组的公共实现。
     *
     * @param group 组播组地址。
     * @param interfaceAddress 网卡地址，可为空。
     * @param option IP_ADD_MEMBERSHIP 或 IP_DROP_MEMBERSHIP。
     * @return 操作成功返回 true，失败返回 false 并记录日志。
     */
    bool updateMulticastMembership(const std::string& group, const std::string& interfaceAddress, int option);

    // 接管已有句柄。
    explicit UDPSocket(SocketHandle handle) : Socket(handle, SOCK_DGRAM, IPPROTO_UDP) {}
};

} // namespace netproxy::net
