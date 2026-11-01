#pragma once

#include <cstdint>
#include <string>

#include "Protocol.h"

namespace netproxy::protocol {

/**
 * 连接帧。
 *
 * 所有帧共用一个连接级序号空间，`streamId` 为 0 表示连接控制。
 * 连接层与流层共用该结构：连接层负责序号、确认与重传，流层只读 `streamId` 与负载。
 */
struct PeerFrame {
    std::uint8_t type = static_cast<std::uint8_t>(FrameType::Data);
    std::uint32_t nodeId = 0;
    std::uint32_t connectionSeq = 0;
    std::uint32_t acknowledge = 0;
    std::uint16_t window = 0;
    std::uint16_t payloadLength = 0;
    std::uint32_t streamId = 0;
    std::string payload;
};

} // namespace netproxy::protocol
