#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>

#include "DataProtocol.h"
#include "net/Socket.h"
#include "protocol/Protocol.h"

namespace netproxy::transport {

// 数据传输配置。
struct TransportConfig {
    std::size_t maxPayloadSize = 1400; // 单包负载上限
    std::size_t sendWindow = 16;       // 发送窗口（报文数）
    std::size_t receiveWindow = 64;    // 接收缓存上限（报文数）
    int retransmitTimeoutMs = 200;     // 重传超时
    int maxRetransmitCount = 5;        // 单包重传上限
    int dataTickMs = 50;               // 重传检查粒度
};

/**
 * 单条会话的可靠数据通道。
 *
 * 负责序号分配、发送窗口、超时重传、乱序缓存与按序交付。
 * 不持有 socket，发送通过构造注入的 Sender 完成；所有状态只在业务线程访问。
 */
class DataStream final {
public:
    // 发送报文，由 DataStreamManager 注入。
    using Sender = std::function<void(const std::string& packet, const net::IPAddress& target)>;

    // 数据按序交付。
    using Delivered = std::function<void(const char* data, std::size_t size)>;

    // 重传耗尽等不可恢复错误。
    using Closed = std::function<void(protocol::CloseReason reason)>;

    /**
     * 构造数据流。
     *
     * @param sessionId 会话标识。
     * @param peerNodeId 对端节点标识。
     * @param peer 对端地址与端口。
     * @param localNodeId 本节点标识，写入报文头。
     * @param config 传输配置。
     * @param sender 报文发送函数。
     */
    DataStream(std::uint32_t sessionId, std::uint32_t peerNodeId, const net::IPAddress& peer, std::uint32_t localNodeId,
               const TransportConfig& config, Sender sender);

    ~DataStream();

    DataStream(const DataStream&) = delete;
    DataStream& operator=(const DataStream&) = delete;

    /**
     * 提交一条消息。
     *
     * 窗口可用时立即分配序号并发送，窗口满时暂存等待 ACK。
     *
     * @param data 消息内容。
     * @param size 长度，超过 maxPayloadSize 返回 false。
     * @return 已受理返回 true；长度非法或流已关闭返回 false。
     */
    bool send(const void* data, std::size_t size);

    /**
     * 处理收到的 Data 报文。
     *
     * 同时处理报文携带的 acknowledge，按序交付并回 ACK。
     */
    void onData(const DataMessage& message);

    /**
     * 处理收到的 Ack 报文。
     */
    void onAck(const DataMessage& message);

    /**
     * 重传检查，由业务线程周期调用。
     *
     * @param now 当前时间。
     */
    void tick(std::chrono::steady_clock::time_point now);

    void setDeliveredHandler(Delivered handler);
    void setClosedHandler(Closed handler);

    /**
     * 获取会话标识。
     *
     * @return 构造时传入的 sessionId。
     */
    std::uint32_t sessionId() const;

    /**
     * 获取对端节点标识。
     *
     * @return 构造时传入的 peerNodeId。
     */
    std::uint32_t peerNodeId() const;

    /**
     * 判断是否已终止。
     *
     * @return 重传耗尽或外部关闭后返回 true。
     */
    bool isClosed() const;

private:
    // 已发送未确认的报文。
    struct PendingSegment {
        std::uint32_t sequence = 0;
        std::string payload;
        std::chrono::steady_clock::time_point sentAt{};
        int retransmitCount = 0;
    };

    // 发送一个已分配序号的报文并刷新发送时间。
    void sendSegment(PendingSegment& segment);

    // 回一个纯 ACK。
    void sendAck();

    // 把接收缓存中的连续段按序交付。
    void deliverContiguous();

    // 用暂存数据填充发送窗口。
    void fillWindow();

    // 终止数据流并回调。
    void close(protocol::CloseReason reason);

    // 剩余接收缓存可通告的报文数。
    std::uint16_t windowRemaining() const;

    std::uint32_t sessionId_ = 0;
    std::uint32_t peerNodeId_ = 0;
    std::uint32_t localNodeId_ = 0;
    net::IPAddress peer_;
    TransportConfig config_;
    Sender sender_;
    Delivered delivered_;
    Closed closedHandler_;

    // 发送侧
    std::uint32_t sendNext_ = 0;
    std::uint32_t sendBase_ = 0;
    std::uint32_t peerWindow_ = 16;
    std::deque<PendingSegment> sendQueue_;
    std::deque<std::string> pendingPayloads_;
    int duplicateAcks_ = 0;

    // 接收侧
    std::uint32_t receiveNext_ = 0;
    std::map<std::uint32_t, std::string> receiveBuffer_;

    bool closed_ = false;
};

} // namespace netproxy::transport
