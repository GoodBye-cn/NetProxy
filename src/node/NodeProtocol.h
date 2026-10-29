#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "net/Socket.h"

namespace netproxy::node {

// 节点间报文类型。
enum class NodeMessageType : std::uint8_t {
    Announce = 1, // 公告自身地址与数据端口
    Bye = 2       // 通知即将离开
};

/**
 * 节点间通信报文，固定 16 字节。
 *
 * 布局：magic(4) + version(1) + type(1) + address(4) + port(2) + nodeId(4)，
 * 全部网络字节序。address 填 0 表示发送方未声明，接收方回退到报文源地址。
 */
class NodeMessage final {
public:
    static constexpr std::size_t SIZE = 16;

    NodeMessageType type = NodeMessageType::Announce;
    net::IPAddress address;   // 发送方声明的可连接地址
    std::uint32_t nodeId = 0; // 发送方节点标识
    std::uint16_t port = 0;   // 发送方数据端口

    /**
     * 编码为网络字节序的报文。
     *
     * @return 长度为 SIZE 的二进制内容；address 无效时写 0。
     */
    std::string encode() const;

    /**
     * 解析报文。
     *
     * 依次校验长度、magic、版本、类型与端口。
     *
     * @param data 报文首地址。
     * @param size 报文长度，小于 SIZE 时解析失败。
     * @param message 解析结果，仅在返回 true 时被写入。
     * @return 解析成功返回 true；任一字段非法返回 false。
     */
    static bool decode(const void* data, std::size_t size, NodeMessage& message);
};

} // namespace netproxy::node
