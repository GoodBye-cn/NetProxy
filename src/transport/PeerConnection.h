#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>

#include "PeerProtocol.h"
#include "TransportTypes.h"
#include "net/Socket.h"
#include "protocol/Protocol.h"

namespace netproxy::transport {

/**
 * 连接与流参数。
 *
 * 所有时间单位为毫秒；窗口类参数单位为「帧数」，不是字节数。
 * 参数在构造时拷贝，运行期不可修改。
 */
struct PeerConfig {
    int helloRetryIntervalMs = 1000;   // Hello 重发间隔；总尝试次数为 1 + helloRetryCount
    int helloRetryCount = 5;           // Hello 最多重发次数，耗尽后连接进入 Closed
    std::size_t maxStreams = 16;       // 单连接流上限，超限回 StreamReject
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
    Connected = 2,  // 握手完成，可以收发帧与流控制
    Closing = 3,    // 已发出 Close，等待对端关闭或超时
    Closed = 4      // 终态，所有流已回调 Closed，不再发送任何报文
};

/**
 * 流状态。
 *
 * ```
 * Opening --StreamAccept--> Open
 *    |                       |
 *    +--StreamReject--> Closed <--StreamClose/连接断开
 * ```
 *
 * 当前实现对本地关闭的流直接从流表移除，`Closing` 预留给「先发关闭、等确认再移除」的优雅关闭。
 */
enum class StreamState : std::uint8_t {
    Opening = 1, // 已发出 StreamOpen，等待对端 StreamAccept
    Open = 2,    // 双向可传数据
    Closing = 3, // 已发出 StreamClose，等待清理（当前实现未使用）
    Closed = 4   // 已关闭，不再出现在流表中
};

/**
 * 一条节点对连接的传输层实现。
 *
 * 职责：
 * - 握手：发送 Hello、校验 HelloAck 的 nonce、处理 HelloReject
 * - 连接级可靠传输：`connectionSeq` 递增、累计 ACK、发送窗口、超时重传、重复 ACK 快速重传
 * - 流多路复用：`StreamOpen / StreamAccept / StreamReject / StreamClose` 推进流状态
 * - 心跳与超时：空闲发 Ping，连续失败关闭连接；无流空闲时主动关闭
 * - 有序交付：按 `connectionSeq` 去重、缓存乱序帧，按序交给 `DataCallback`
 *
 * 不负责：
 * - socket 的创建、读写与关闭（由 `DataChannel` 持有）
 * - 对端是否存在、地址查询（由 `PeerConnectionManager` 与上层决定）
 * - 业务语义（流 ID 由调用方分配，传输层不区分用途）
 *
 * 线程模型：
 * - 全部状态只在业务线程访问，不加锁
 * - `open / onFrame / onHelloAck / tick / openStream / closeStream / sendData` 都由
 *   `PeerConnectionManager` 在业务线程调用
 * - `Sender` 回调把报文交给 `DataChannel::send`，后者内部投递到 I/O 线程
 *
 * 序号规则：
 * - `Data` 与 `Stream*` 帧参与序号与重传
 * - `Ping` 与 `Close` 是连接控制帧：`streamId = 0`、`connectionSeq = 0`，不占交付序号，
 *   避免纯 ACK 丢失导致 `receiveNext_` 卡住
 */
class PeerConnection final {
public:
    /**
     * 发送报文。
     *
     * @param packet 已编码的报文。
     * @param target 目标地址。
     * @note 由管理器注入，通常直接转 `DataChannel::send`；不得在回调中阻塞。
     */
    using Sender = PacketSender;

    /**
     * 流事件回调。
     *
     * @param streamId 流标识。
     * @param event Opened / Rejected / Closed。
     * @param reason 拒绝或关闭原因；Opened 时为 0。
     */
    using StreamCallback =
        std::function<void(std::uint32_t streamId, protocol::StreamEvent event, protocol::Reason reason)>;

    /**
     * 数据交付回调。
     *
     * @param streamId 所属流。
     * @param data 负载首地址，回调返回后失效。
     * @param size 负载长度，不超过 maxPayloadSize。
     */
    using DataCallback = std::function<void(std::uint32_t streamId, const char* data, std::size_t size)>;

    /**
     * 构造连接，不发送任何报文。
     *
     * @param peerNodeId 对端节点标识，作为连接唯一键。
     * @param localNodeId 本节点标识，写入每条报文的头部。
     * @param peer 对端地址与端口。
     * @param config 连接参数，构造时拷贝。
     * @param sender 报文发送函数。
     * @note 构造后处于 Connecting，需要调用 open() 或 acceptHandshake() 才会推进状态。
     */
    PeerConnection(std::uint32_t peerNodeId, std::uint32_t localNodeId, const net::IPAddress& peer,
                   const PeerConfig& config, Sender sender);

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
     * 收到 Hello 并回 HelloAck 后由管理器调用；不校验 nonce，因为 nonce 是对方生成并回显的。
     */
    void acceptHandshake();

    /**
     * 处理 HelloReject。
     *
     * 连接直接进入 Closed，所有已存在的流回调 Closed。
     * 收到的 reason 只用于日志，不向上层透传。
     *
     * @param message 已解析的握手报文。
     */
    void onHelloReject(const HelloMessage& message);

    /**
     * 处理连接帧。
     *
     * 先处理帧携带的 acknowledge，再按类型分发：
     * - `Ping`：仅刷新活动时间，不进入有序交付
     * - `Close`：关闭连接与所有流
     * - 其余：按 `connectionSeq` 去重、缓存、按序交付
     *
     * 状态不是 Connected / Closing 时直接丢弃。
     *
     * @param frame 已解析的帧。
     */
    void onFrame(const PeerFrame& frame);

    /**
     * 周期检查。
     *
     * 按当前状态执行：
     * - Connecting：Hello 重发与超时
     * - Connected：队首重传、心跳、无流空闲关闭
     * - Closing：等待超时后进入 Closed
     *
     * @param now 当前时间，由管理器统一传入，便于测试注入。
     */
    void tick(std::chrono::steady_clock::time_point now);

    /**
     * 发起一条流。
     *
     * 要求连接已 Connected、streamId 非 0、流不存在且未超上限。
     * 成功后流进入 Opening 并发送 StreamOpen，最终由 `StreamCallback` 通知 Opened 或 Rejected。
     *
     * @param streamId 流标识，非 0，由调用方保证唯一。
     * @return 已进入 Opening 返回 true。
     */
    bool openStream(std::uint32_t streamId);

    /**
     * 关闭一条流。
     *
     * 发送 StreamClose 后立即从流表移除，并回调 `Closed(Local)`。
     * 对端收到后同样移除并回调 `Closed(Remote)`。
     *
     * @param streamId 流标识。
     * @return 流存在并已发出关闭帧返回 true；流不存在返回 false。
     */
    bool closeStream(std::uint32_t streamId);

    /**
     * 向指定流提交数据。
     *
     * 要求连接 Connected、流状态为 Open、长度在 1~maxPayloadSize 之间。
     * 窗口可用时立即发送，否则进入 `pendingFrames_`，由 ACK 推进后补发。
     *
     * @param streamId 流标识。
     * @param data 数据内容。
     * @param size 长度，1~maxPayloadSize。
     * @return 已受理返回 true；连接未建立、流未打开或长度非法返回 false。
     * @note 返回 true 只表示本地受理，不代表对端收到。
     */
    bool sendData(std::uint32_t streamId, const void* data, std::size_t size);

    /**
     * 关闭连接。
     *
     * 发送 Close 控制帧，清空发送队列与接收缓存，所有流回调 `Closed(reason)`。
     * 已是 Closed 时不做任何事，保证回调只触发一次。
     *
     * @param reason 关闭原因，透传给流回调。
     */
    void close(protocol::Reason reason);

    // 是否处于 Connected。
    bool isConnected() const;

    // 当前连接状态。
    ConnectionState state() const;

    // 对端节点标识。
    std::uint32_t peerNodeId() const;

    // 设置流事件回调；为空表示不通知。
    void setStreamHandler(StreamCallback handler);

    // 设置数据交付回调；为空表示丢弃数据。
    void setDataHandler(DataCallback handler);

private:
    // 已发送未确认的帧。
    struct PendingFrame {
        PeerFrame frame;                                // 已分配序号的帧
        std::chrono::steady_clock::time_point sentAt{}; // 最近一次发送时间，用于 RTO
        int retransmitCount = 0;                        // 已重传次数
    };

    // 窗口可用时立即分配序号并发送，否则进入 pendingFrames_。
    void queueFrame(PeerFrame frame);

    // 分配 connectionSeq，填充 acknowledge 与 window，入队并发送。
    void sendFrame(PeerFrame frame);

    // 窗口有空余时，把 pendingFrames_ 中的帧依次发出。
    void flushWindow();

    // 处理累计确认：推进 sendBase、更新对端窗口；连续 3 个重复 ACK 触发快速重传。
    void handleAck(std::uint32_t acknowledge, std::uint16_t window);

    // 按 connectionSeq 去重、缓存乱序帧，并按序交付。
    void receiveFrame(const PeerFrame& frame);

    // 交付一帧：Data 交给上层，Stream* 推进流状态，Close 关闭连接。
    void deliverFrame(const PeerFrame& frame);

    // 处理 StreamOpen / StreamAccept / StreamReject / StreamClose。
    void handleStreamFrame(const PeerFrame& frame);

    // 回一个纯 ACK（Ping 帧承载 acknowledge，不占交付序号）。
    void sendAck();

    // 发送连接级控制帧（Ping / Close），connectionSeq 与 streamId 均为 0。
    void sendControl(protocol::FrameType type, const std::string& payload);

    // 连接断开时逐个回调流 Closed 并清空流表。
    void closeAllStreams(protocol::Reason reason);

    // 通知单个流事件。
    void emitStreamEvent(std::uint32_t streamId, protocol::StreamEvent event, protocol::Reason reason);

    // 剩余接收缓存，作为通告 window；缓存满时返回 0。
    std::uint16_t windowRemaining() const;

    // 构造并发送 Hello。
    void sendHello();

    // 连接标识与对端信息
    std::uint32_t peerNodeId_ = 0;  // 对端节点标识
    std::uint32_t localNodeId_ = 0; // 本节点标识
    net::IPAddress peer_;           // 对端地址与端口
    PeerConfig config_;             // 连接参数
    Sender sender_;                 // 报文发送函数

    // 握手状态
    ConnectionState state_ = ConnectionState::Connecting; // 当前状态
    bool handshakeStarted_ = false;                       // 是否已发出首个 Hello
    std::uint32_t helloNonce_ = 0;                        // 本地 nonce，用于校验 HelloAck
    int helloRetries_ = 0;                                // 已重发次数
    std::chrono::steady_clock::time_point helloSentAt_{}; // 最近一次发送 Hello 的时间

    // 发送侧
    std::uint32_t sendNext_ = 0;          // 下一个待分配序号
    std::uint32_t sendBase_ = 0;          // 最老的未确认序号，等待该序号之前的帧被确认
    std::uint32_t peerWindow_ = 16;       // 对端通告的窗口，初始取 sendWindow
    std::deque<PendingFrame> sendQueue_;  // 已分配序号、等待确认的帧，按序号升序
    std::deque<PeerFrame> pendingFrames_; // 窗口满时暂存，尚未分配序号
    int duplicateAcks_ = 0;               // 连续重复 ACK 次数，达到 3 触发快速重传

    // 接收侧
    std::uint32_t receiveNext_ = 0;                    // 期望收到的下一个序号
    std::map<std::uint32_t, PeerFrame> receiveBuffer_; // 乱序缓存，上限 receiveWindow

    // 流表：只保留未关闭的流
    std::map<std::uint32_t, StreamState> streams_;

    // 心跳与超时
    int pingMisses_ = 0;                                   // 连续未响应次数
    std::chrono::steady_clock::time_point pingSentAt_{};   // 最近一次发送 Ping 的时间
    std::chrono::steady_clock::time_point lastActive_{};   // 最近一次收到任意帧的时间
    std::chrono::steady_clock::time_point closingSince_{}; // 进入 Closing 的时间

    // 回调
    StreamCallback streamHandler_;
    DataCallback dataHandler_;
};

} // namespace netproxy::transport
