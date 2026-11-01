#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>

#include "PeerConfig.h"
#include "PeerProtocol.h"
#include "Protocol.h"
#include "Socket.h"
#include "TransportTypes.h"

namespace netproxy::transport {

/**
 * 连接状态。
 *
 * ```
 * Connecting --HelloAck/acceptHandshake--> Connected
 *      |                                      |
 *      +--重试耗尽/HelloReject--> Closed <----+--Close/心跳失败/空闲超时
 * ```
 */
enum class ConnectionState : std::uint8_t {
    Connecting = 1, // 已发出 Hello，等待 HelloAck；此时不能收发连接帧
    Connected = 2,  // 握手完成，可以收发帧
    Closing = 3,    // 已发出 Close，等待对端关闭或超时
    Closed = 4      // 终态，不再发送任何报文
};

/**
 * 连接层：一条节点对连接的可靠有序帧通道。
 *
 * 职责：
 * - 握手：发送 Hello、校验 HelloAck 的 nonce、处理 HelloReject
 * - 可靠传输：`connectionSeq` 递增、累计 ACK、发送窗口、超时重传、重复 ACK 快速重传
 * - 有序交付：按 `connectionSeq` 去重、缓存乱序帧，按序交给 `FrameHandler`
 * - 心跳与超时：空闲发 Ping，连续失败关闭连接
 *
 * 不负责：
 * - 流 semantics（流表、StreamOpen/Accept/Reject/Close、maxStreams），由流层负责
 * - socket 的创建与读写（由 `DataChannel` 持有）
 *
 * 线程模型：全部状态只在业务线程访问，不加锁。
 *
 * 序号规则：`Ping` / `Close` 是控制帧，不占交付序号；其余帧（含流信令）走同一序号空间。
 */
class PeerConnection final {
public:
    /**
     * 发送报文。
     *
     * @param packet 已编码的报文。
     * @param target 目标地址。
     * @note 由组装层注入，通常直接转 `DataChannel::send`；不得在回调中阻塞。
     */
    using Sender = PacketSender;

    // 有序交付的帧（Data 与 Stream* 帧）。
    using FrameHandler = std::function<void(const protocol::PeerFrame&)>;

    // 连接进入 Closed 时触发，保证只触发一次。
    using CloseHandler = std::function<void(protocol::Reason)>;

    // 查询「流表是否为空」，用于空闲关闭；由流层提供。
    using HasStreams = std::function<bool()>;

    /**
     * 构造连接，不发送任何报文。
     *
     * @param peerNodeId 对端节点标识，作为连接唯一键。
     * @param localNodeId 本节点标识，写入每条报文的头部。
     * @param peer 对端地址与端口。
     * @param config 连接参数，构造时拷贝。
     * @param sender 报文发送函数。
     * @param hasStreams 活跃流判定；为空时按「无流」处理。
     * @note 构造后处于 Connecting，需要调用 open() 或 acceptHandshake() 才会推进状态。
     */
    PeerConnection(std::uint32_t peerNodeId, std::uint32_t localNodeId, const net::IPAddress& peer,
                   const PeerConfig& config, Sender sender, HasStreams hasStreams = {});

    // 析构不发送 Close，调用方应先调用 close() 通知对端。
    ~PeerConnection();

    PeerConnection(const PeerConnection&) = delete;
    PeerConnection& operator=(const PeerConnection&) = delete;

    /**
     * 发起握手：生成 nonce、发送 Hello，进入 Connecting。
     *
     * 只允许调用一次：重复调用返回 false，不会重置 nonce 或重发。
     *
     * @return 已发出首个 Hello 返回 true；已发起过或状态不是 Connecting 返回 false。
     */
    bool open();

    /**
     * 处理 HelloAck。
     *
     * 校验 nonce 一致后进入 Connected，并补发窗口内的排队帧。
     * 状态不是 Connecting、或 nonce 不匹配时忽略。
     *
     * @param message 已解析的握手报文。
     */
    void onHelloAck(const HelloMessage& message);

    /**
     * 作为接收方确认握手，直接进入 Connected。
     *
     * 收到 Hello 并回 HelloAck 后由组装层调用；不校验 nonce，因为 nonce 是对方生成并回显的。
     */
    void acceptHandshake();

    /**
     * 处理 HelloReject。
     *
     * 连接直接进入 Closed，并触发 `CloseHandler(Error)`。
     *
     * @param message 已解析的握手报文。
     */
    void onHelloReject(const HelloMessage& message);

    /**
     * 处理连接帧。
     *
     * 先处理帧携带的 acknowledge，再按类型分发：
     * - `Ping`：仅刷新活动时间，不进入有序交付
     * - `Close`：进入 Closed 并触发 `CloseHandler(Remote)`
     * - 其余：按 `connectionSeq` 去重、缓存、按序交给 `FrameHandler`
     *
     * 状态不是 Connected / Closing 时直接丢弃。
     *
     * @param frame 已解析的帧。
     */
    void onFrame(const protocol::PeerFrame& frame);

    /**
     * 提交一帧，走可靠有序通道。
     *
     * 要求状态为 Connected、负载不超过 `maxPayloadSize`；窗口不足时内部排队。
     *
     * @param frame 已填好 type / streamId / payload 的帧。
     * @return 已受理返回 true；连接不可用或负载超限返回 false。
     * @note 返回 true 只表示本地受理，不代表对端收到。
     */
    bool send(protocol::PeerFrame frame);

    /**
     * 周期检查。
     *
     * 按当前状态执行：
     * - Connecting：Hello 重发与超时
     * - Connected：队首重传、心跳、无流空闲关闭
     * - Closing：等待超时后进入 Closed
     *
     * @param now 当前时间，由组装层统一传入，便于测试注入。
     */
    void tick(std::chrono::steady_clock::time_point now);

    /**
     * 关闭连接。
     *
     * 发送 Close 控制帧，清空发送队列与接收缓存，触发 `CloseHandler(reason)`。
     * 已是 Closed 时不做任何事，保证回调只触发一次。
     *
     * @param reason 关闭原因，透传给上层。
     */
    void close(protocol::Reason reason);

    // 是否处于 Connected。
    bool isConnected() const;

    // 当前连接状态；仅业务线程可读。
    ConnectionState state() const;

    // 对端节点标识。
    std::uint32_t peerNodeId() const;

    // 对端地址。
    const net::IPAddress& peer() const;

    // 单帧负载上限。
    std::size_t maxPayloadSize() const;

    // 设置有序交付回调；为空表示丢弃。
    void setFrameHandler(FrameHandler handler);

    // 设置关闭回调；为空表示不通知。
    void setCloseHandler(CloseHandler handler);

private:
    // 已发送未确认的帧。
    struct PendingFrame {
        protocol::PeerFrame frame;                                // 已分配序号的帧
        std::chrono::steady_clock::time_point sentAt{}; // 最近一次发送时间，用于 RTO
        int retransmitCount = 0;                        // 已重传次数
    };

    // 窗口可用时立即分配序号并发送，否则进入 pendingFrames_。
    void queueFrame(protocol::PeerFrame frame);

    // 分配 connectionSeq，填充 acknowledge 与 window，入队并发送。
    void sendFrame(protocol::PeerFrame frame);

    // 窗口有空余时，把 pendingFrames_ 中的帧依次发出。
    void flushWindow();

    // 处理累计确认：推进 sendBase、更新对端窗口；连续 3 个重复 ACK 触发快速重传。
    void handleAck(std::uint32_t acknowledge, std::uint16_t window);

    // 按 connectionSeq 去重、缓存乱序帧，并按序交付。
    void receiveFrame(const protocol::PeerFrame& frame);

    // 把帧交给 FrameHandler（已在有序位置）。
    void deliverOrdered(const protocol::PeerFrame& frame);

    // 回一个纯 ACK（Ping 帧承载 acknowledge，不占交付序号）。
    void sendAck();

    // 发送连接级控制帧（Ping / Close），connectionSeq 与 streamId 均为 0。
    void sendControl(protocol::FrameType type, const std::string& payload);

    // 剩余接收缓存，作为通告 window；缓存满时返回 0。
    std::uint16_t windowRemaining() const;

    // 构造并发送 Hello。
    void sendHello();

    // 进入 Closed 的唯一出口：清队列、置状态、触发 CloseHandler。
    void enterClosed(protocol::Reason reason);

    // 连接标识与对端信息
    std::uint32_t peerNodeId_ = 0;  // 对端节点标识
    std::uint32_t localNodeId_ = 0; // 本节点标识
    net::IPAddress peer_;           // 对端地址与端口
    PeerConfig config_;             // 连接参数
    Sender sender_;                 // 报文发送函数
    HasStreams hasStreams_;   // 流表非空判定，可为空

    // 握手状态
    ConnectionState state_ = ConnectionState::Connecting; // 当前状态
    bool handshakeStarted_ = false;                       // 是否已发出首个 Hello
    std::uint32_t helloNonce_ = 0;                        // 本地 nonce，用于校验 HelloAck
    int helloRetries_ = 0;                                // 已重发次数
    std::chrono::steady_clock::time_point helloSentAt_{}; // 最近一次发送 Hello 的时间

    // 发送侧
    std::uint32_t sendNext_ = 0;          // 下一个待分配序号
    std::uint32_t sendBase_ = 0;          // 最老的未确认序号
    std::uint32_t peerWindow_ = 16;       // 对端通告的窗口，初始取 sendWindow
    std::deque<PendingFrame> sendQueue_;  // 已分配序号、等待确认的帧，按序号升序
    std::deque<protocol::PeerFrame> pendingFrames_; // 窗口满时暂存，尚未分配序号
    int duplicateAcks_ = 0;               // 连续重复 ACK 次数，达到 3 触发快速重传

    // 接收侧
    std::uint32_t receiveNext_ = 0;                    // 期望收到的下一个序号
    std::map<std::uint32_t, protocol::PeerFrame> receiveBuffer_; // 乱序缓存，上限 receiveWindow

    // 心跳与超时
    int pingMisses_ = 0;                                   // 连续未响应次数
    std::chrono::steady_clock::time_point pingSentAt_{};   // 最近一次发送 Ping 的时间
    std::chrono::steady_clock::time_point lastActive_{};   // 最近一次收到任意帧的时间
    std::chrono::steady_clock::time_point closingSince_{}; // 进入 Closing 的时间

    // 回调
    FrameHandler frameHandler_;
    CloseHandler closeHandler_;
};

} // namespace netproxy::transport
