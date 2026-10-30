#pragma once

#include <functional>
#include <string>

#include "net/Socket.h"

namespace netproxy::transport {

/**
 * 把一段已编码的报文发送到指定地址。
 *
 * 由 `DataChannel` 提供实现，注入给 `PeerConnection`，避免连接层直接持有 socket。
 */
using PacketSender = std::function<void(const std::string& packet, const net::IPAddress& target)>;

} // namespace netproxy::transport
