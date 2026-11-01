#pragma once

#include <cstddef>

namespace netproxy::transport {

/**
 * 流层参数。
 *
 * 与 `PeerConfig` 分开：连接参数描述可靠通道，流参数描述多路复用策略。
 */
struct StreamConfig {
    std::size_t maxStreams = 16; // 单连接流上限，超限回 StreamReject
    int acceptTimeoutMs = 0;     // 预留：等待 StreamAccept 的超时，0 表示不检查
};

} // namespace netproxy::transport
