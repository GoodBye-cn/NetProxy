#pragma once

#include <cstddef>

namespace netproxy::transport {

/**
 * 连接参数。
 *
 * 所有时间单位为毫秒；窗口类参数单位为「帧数」，不是字节数。
 * 参数在构造时拷贝，运行期不可修改。
 *
 * 流相关参数（单连接流上限等）在 `stream/StreamConfig.h`。
 */
struct PeerConfig {
    int helloRetryIntervalMs = 1000;   // Hello 重发间隔；总尝试次数为 1 + helloRetryCount
    int helloRetryCount = 5;           // Hello 最多重发次数，耗尽后连接进入 Closed
    std::size_t maxPayloadSize = 1400; // 单帧负载上限，需小于 MTU 以免 IP 分片
    std::size_t sendWindow = 16;       // 发送窗口：未确认帧的数量上限
    std::size_t receiveWindow = 64;    // 接收缓存上限，同时作为通告窗口的最大值
    int retransmitTimeoutMs = 200;     // 重传超时（RTO），只重发队首帧
    int maxRetransmitCount = 5;        // 单帧重传次数上限，超过判定连接错误
    int pingIntervalMs = 30000;        // 空闲达到该时间后发送心跳 Ping
    int pingTimeoutMs = 10000;         // 每次 Ping 的等待时间，超过且无任何入站帧则计一次失败
    int pingRetryCount = 3;            // 连续心跳失败上限，达到后关闭连接
    int idleTimeoutMs = 60000;         // 无流且空闲达到该时间时主动关闭连接
    int closeTimeoutMs = 2000;         // 发出 Close 后等待的时长，超时直接进入 Closed
    int tickMs = 50;                   // 重传、心跳与超时的检查粒度，由管理器调度
};

} // namespace netproxy::transport
