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
    std::uint8_t type = static_cast<std::uint8_t>(FrameType::Data); // FrameType，10~16
    std::uint32_t nodeId = 0;                                       // 发送方节点标识
    std::uint32_t connectionSeq = 0;                                // 连接级序号；Ping / Close 为 0
    std::uint32_t acknowledge = 0;                                  // 累计确认：期望收到的下一个序号
    std::uint16_t window = 0;                                       // 接收方剩余缓存，单位是帧数
    std::uint16_t payloadLength = 0;                                // 负载长度，解码时等于 payload.size()
    std::uint32_t streamId = 0;                                     // 0 表示连接控制帧
    std::string payload;                                            // 帧负载
};

} // namespace netproxy::protocol
