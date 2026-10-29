#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>

#include "PeerProtocol.h"
#include "net/Socket.h"
#include "protocol/Protocol.h"

namespace netproxy::transport {

// 连接与流配置。
struct PeerConfig {
    int helloRetryIntervalMs = 1000;   // Hello 重发间隔
    int helloRetryCount = 5;           // Hello 最多重发次数
    std::size_t maxStreams = 16;       // 单连接流上限
    std::size_t maxPayloadSize = 1400; // 单帧负载上限
    std::size_t sendWindow = 16;       // 发送窗口（帧数）
    std::size_t receiveWindow = 64;    // 接收缓存（帧数）
    int retransmitTimeoutMs = 200;     // 重传超时
    int maxRetransmitCount = 5;        // 单帧重传上限
    int pingIntervalMs = 30000;        // 心跳间隔
    int pingTimeoutMs = 10000;         // 心跳等待时间
    int pingRetryCount = 3;            // 连续失败上限
    int idleTimeoutMs = 60000;         // 无流空闲关闭连接
    int closeTimeoutMs = 2000;         // 关闭等待时间
    int tickMs = 50;                   // 检查粒度
};

// 连接状态。
enum class ConnectionState : std::uint8_t { Connecting = 1, Connected = 2, Closing = 3, Closed = 4 };

// 流状态。
enum class StreamState : std::uint8_t { Opening = 1, Open = 2, Closing = 3, Closed = 4 };

/**
 * 一条节点对连接的传输层实现。
 *
 * 负责握手、连接级序号与窗口、重传、心跳与流表；不持有 socket，
 * 发送通过构造注入的 Sender 完成。所有状态只在业务线程访问。
 */
class PeerConnection final {
public:
    // 发送报文，由 PeerConnectionManager 注入。
    using Sender = std::function<void(const std::string& packet, const net::IPAddress& target)>;

    // 流事件：opened / rejected / closed。
    using StreamCallback =
        std::function<void(std::uint32_t streamId, protocol::StreamEvent event, protocol::Reason reason)>;

    // 数据按序交付。
    using DataCallback = std::function<void(std::uint32_t streamId, const char* data, std::size_t size)>;

    /**
     * 构造连接，不发送任何报文。
     *
     * @param peerNodeId 对端节点标识。
     * @param localNodeId 本节点标识。
     * @param peer 对端地址与端口。
     * @param config 连接参数。
     * @param sender 报文发送函数。
     */
    PeerConnection(std::uint32_t peerNodeId, std::uint32_t localNodeId, const net::IPAddress& peer,
                   const PeerConfig& config, Sender sender);

    ~PeerConnection();

    PeerConnection(const PeerConnection&) = delete;
    PeerConnection& operator=(const PeerConnection&) = delete;

    /**
     * 发起握手，发送 Hello 并进入 Connecting。
     *
     * @return 报文已发出返回 true。
     */
    bool open();

    /**
     * 处理 HelloAck。
     *
     * @param message 握手报文。
     */
    void onHelloAck(const HelloMessage& message);

    /**
     * 作为接收方确认握手，直接进入 Connected。
     *
     * 收到 Hello 并回 HelloAck 后调用。
     */
    void acceptHandshake();

    /**
     * 处理 HelloReject。
     *
     * @param message 握手报文。
     */
    void onHelloReject(const HelloMessage& message);

    /**
     * 处理连接帧。
     *
     * @param frame 已解析的帧。
     */
    void onFrame(const PeerFrame& frame);

    /**
     * 周期检查：握手重试、重传、心跳与空闲关闭。
     *
     * @param now 当前时间。
     */
    void tick(std::chrono::steady_clock::time_point now);

    /**
     * 发起一条流。
     *
     * @param streamId 流标识，非 0。
     * @return 已进入 Opening 返回 true；连接未建立、流已存在或超限返回 false。
     */
    bool openStream(std::uint32_t streamId);

    /**
     * 关闭一条流。
     *
     * @param streamId 流标识。
     * @return 流存在并已发出关闭帧返回 true。
     */
    bool closeStream(std::uint32_t streamId);

    /**
     * 向指定流提交数据。
     *
     * @param streamId 流标识。
     * @param data 数据内容。
     * @param size 长度，1~1400。
     * @return 已受理返回 true。
     */
    bool sendData(std::uint32_t streamId, const void* data, std::size_t size);

    /**
     * 关闭连接，通知所有流。
     */
    void close(protocol::Reason reason);

    bool isConnected() const;
    ConnectionState state() const;
    std::uint32_t peerNodeId() const;

    void setStreamHandler(StreamCallback handler);
    void setDataHandler(DataCallback handler);

private:
    // 已发送未确认的帧。
    struct PendingFrame {
        PeerFrame frame;
        std::chrono::steady_clock::time_point sentAt{};
        int retransmitCount = 0;
    };

    // 窗口可用时立即发送，否则排队。
    void queueFrame(PeerFrame frame);

    // 分配序号并立即发送。
    void sendFrame(PeerFrame frame);

    // 窗口可用时发送排队帧。
    void flushWindow();

    // 处理累计确认与重复 ACK。
    void handleAck(std::uint32_t acknowledge, std::uint16_t window);

    // 按连接序号去重、缓存并交付。
    void receiveFrame(const PeerFrame& frame);

    // 交付一帧：数据交给上层，控制帧推进流状态。
    void deliverFrame(const PeerFrame& frame);

    // 处理流控制帧。
    void handleStreamFrame(const PeerFrame& frame);

    // 回一个纯 ACK。
    void sendAck();

    // 发送连接级控制帧（Ping / Close），不占交付序号。
    void sendControl(protocol::FrameType type, const std::string& payload);

    // 关闭所有流并回调。
    void closeAllStreams(protocol::Reason reason);

    // 通知流事件。
    void emitStreamEvent(std::uint32_t streamId, protocol::StreamEvent event, protocol::Reason reason);

    // 剩余接收缓存。
    std::uint16_t windowRemaining() const;

    // 发送 Hello。
    void sendHello();

    std::uint32_t peerNodeId_ = 0;
    std::uint32_t localNodeId_ = 0;
    net::IPAddress peer_;
    PeerConfig config_;
    Sender sender_;

    ConnectionState state_ = ConnectionState::Connecting;
    bool handshakeStarted_ = false;
    std::uint32_t helloNonce_ = 0;
    int helloRetries_ = 0;
    std::chrono::steady_clock::time_point helloSentAt_{};

    // 发送侧
    std::uint32_t sendNext_ = 0;
    std::uint32_t sendBase_ = 0;
    std::uint32_t peerWindow_ = 16;
    std::deque<PendingFrame> sendQueue_;
    std::deque<PeerFrame> pendingFrames_;
    int duplicateAcks_ = 0;

    // 接收侧
    std::uint32_t receiveNext_ = 0;
    std::map<std::uint32_t, PeerFrame> receiveBuffer_;

    // 流表
    std::map<std::uint32_t, StreamState> streams_;

    // 心跳与空闲
    int pingMisses_ = 0;
    std::chrono::steady_clock::time_point pingSentAt_{};
    std::chrono::steady_clock::time_point lastActive_{};
    std::chrono::steady_clock::time_point closingSince_{};

    StreamCallback streamHandler_;
    DataCallback dataHandler_;
};

} // namespace netproxy::transport
