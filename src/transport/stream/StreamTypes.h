#pragma once

#include <cstdint>

namespace netproxy::transport {

/**
 * 流状态。
 *
 * ```
 * Opening --StreamAccept--> Open
 *    |                       |
 *    +--StreamReject--> Closed <--StreamClose/连接断开
 * ```
 */
enum class StreamState : std::uint8_t {
    Opening = 1, // 本地已发 StreamOpen，等待对端 StreamAccept
    Open = 2,    // 双向可传数据
    Closing = 3, // 预留：已发 StreamClose，等待清理（当前实现未使用）
    Closed = 4   // 已关闭，不再出现在流表中
};

// 流的对外快照。
struct StreamRecord {
    std::uint32_t streamId = 0;    // 流标识
    std::uint32_t peerNodeId = 0;  // 所属对端
    bool open = false;             // 是否已进入 Open
    bool locallyInitiated = false; // 是否本端发起
};

} // namespace netproxy::transport
