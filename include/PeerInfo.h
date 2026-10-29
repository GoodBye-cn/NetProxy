#pragma once

#include <cstdint>
#include <string>

namespace netproxy {

// 已发现的节点信息。
struct PeerInfo {
    std::string address;      // 数字形式的 IPv4 地址
    std::uint16_t port = 0;   // 对端数据端口，也是会话端口
    std::uint32_t nodeId = 0; // 对端节点标识
};

} // namespace netproxy
