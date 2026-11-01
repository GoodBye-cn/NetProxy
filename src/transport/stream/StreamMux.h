#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Frame.h"
#include "Protocol.h"
#include "Stream.h"
#include "StreamConfig.h"
#include "StreamTypes.h"

namespace netproxy::transport {

/**
 * 流层：单连接上的流表与流信令。
 *
 * 职责：
 * - 唯一的流表（不再在组装层维护镜像）
 * - `StreamOpen / StreamAccept / StreamReject / StreamClose` 的收发与状态推进
 * - `Data` 帧按 `streamId` 分发
 * - 并发上限与「连接未就绪时开流排队」
 *
 * 不负责：
 * - 可靠性（序号、ACK、窗口、重传）与有序交付——由连接层保证
 * - 原始报文编解码与发送
 *
 * 线程模型：状态由 `mutex_` 保护；**不得在持锁期间调用任何外部回调**
 * （`StreamHandler` / `DataHandler` / `FrameSender`），避免应用回调里重入 `sendData` 造成自锁。
 */
class StreamMux final {
public:
    // 流事件：附带对端标识后转发给组装层。
    using StreamHandler = std::function<void(std::uint32_t streamId, std::uint32_t peerNodeId,
                                             protocol::StreamEvent event, protocol::CloseReason reason)>;

    // 数据交付。
    using DataHandler =
        std::function<void(std::uint32_t streamId, std::uint32_t peerNodeId, const char* data, std::size_t size)>;

    // 注入连接层的可靠发送；返回 false 表示连接不可用或负载超限。
    using FrameSender = std::function<bool(protocol::PeerFrame)>;

    /**
     * 构造流层，不创建任何流。
     *
     * @param peerNodeId 所属对端节点标识。
     * @param config 流层参数，构造时拷贝。
     * @param sender 可靠发送函数，必须比本对象活得久。
     */
    StreamMux(std::uint32_t peerNodeId, const StreamConfig& config, FrameSender sender);

    StreamMux(const StreamMux&) = delete;
    StreamMux& operator=(const StreamMux&) = delete;

    /**
     * 处理一条已按序到达的流层帧。
     *
     * @param frame 已解析的帧；只处理 `Data` 与 `Stream*`，其余忽略。
     * @note 只允许在业务线程调用；回调在锁外触发。
     */
    void onFrame(const protocol::PeerFrame& frame);

    /**
     * 通知流层连接已就绪。
     *
     * @note 会补发 `pending_` 中排队的 `StreamOpen`；重复调用无副作用。
     */
    void onConnectionUp();

    /**
     * 通知流层连接已断开。
     *
     * @param reason 关闭原因，透传给已开流的 `Closed` 事件。
     * @note 排队中的流回调 `Rejected(Timeout)`，已发出的流回调 `Closed(reason)`；随后清空流表。
     */
    void onConnectionClosed(protocol::CloseReason reason);

    /**
     * 建一条本地发起的流。
     *
     * @param streamId 流标识，由调用线程生成，非 0。
     * @return 建流受理返回 true；流号重复、超过 `maxStreams` 或发送失败返回 false。
     * @note 连接未就绪时流仍建表并排队，等 `onConnectionUp()` 补发。
     */
    bool openStream(std::uint32_t streamId);

    /**
     * 向指定流提交数据。
     *
     * @param streamId 流标识。
     * @param data 数据首地址，调用返回后可释放。
     * @param size 长度，必须大于 0。
     * @return 流已 Open 且连接受理返回 true；流不存在、未打开或连接拒绝返回 false。
     * @note 长度上限由连接层校验，本层只透传结果。
     */
    bool sendData(std::uint32_t streamId, const void* data, std::size_t size);

    /**
     * 关闭一条流。
     *
     * @param streamId 流标识。
     * @return 流存在并已处理返回 true；流不存在返回 false。
     * @note 排队中的流不向对端发 `StreamClose`（对端尚不知道该流）。
     */
    bool closeStream(std::uint32_t streamId);

    /**
     * 判断是否还有任何流。
     *
     * @return 流表非空返回 true；包含 Opening 与排队中的流。
     * @note 供连接层判定空闲关闭；任意线程可调用。
     */
    bool hasStreams() const;

    /**
     * 判断流是否已打开。
     *
     * @param streamId 流标识。
     * @return 流存在且状态为 Open 返回 true。
     */
    bool isStreamOpen(std::uint32_t streamId) const;

    /**
     * 判断流是否属于本会话。
     *
     * @param streamId 流标识。
     * @return 流在流表中返回 true。
     */
    bool ownsStream(std::uint32_t streamId) const;

    /**
     * 获取全部流。
     *
     * @return 按流号整理的快照，含发起方向与打开状态。
     */
    std::vector<StreamRecord> streams() const;

    /**
     * 注册流事件回调。
     *
     * @param handler 回调对象；为空表示不通知。
     * @note 任意线程可调用，内部加锁替换。
     */
    void setStreamHandler(StreamHandler handler);

    /**
     * 注册数据回调。
     *
     * @param handler 回调对象；为空表示丢弃数据。
     * @note 任意线程可调用，内部加锁替换。
     */
    void setDataHandler(DataHandler handler);

private:
    // 锁外触发的流事件。
    struct Emit {
        std::uint32_t streamId = 0;                                           // 流标识
        protocol::StreamEvent event = protocol::StreamEvent::Opened;          // 事件类型
        protocol::CloseReason reason = static_cast<protocol::CloseReason>(0); // 关闭原因
    };

    // 收到对端开流：建表并回 Accept；已存在只补发 Accept；超限回 Reject。
    void handleStreamOpen(const protocol::PeerFrame& frame);

    // 收到接受：仅当本地流仍为 Opening 时置 Open 并回调 Opened。
    void handleStreamAccept(const protocol::PeerFrame& frame);

    // 收到拒绝：删流并回调 Rejected(Error)。
    void handleStreamReject(const protocol::PeerFrame& frame);

    // 收到关闭：删流并回调 Closed(Remote)。
    void handleStreamClose(const protocol::PeerFrame& frame);

    // 收到数据：按流号分发；找不到流则计数后丢弃。
    void handleData(const protocol::PeerFrame& frame);

    // 发送 StreamOpen；失败时删流并回调 Rejected(Error)。
    bool sendStreamOpen(std::uint32_t streamId);

    // 把 pending_ 中的流全部补发（onConnectionUp 内部使用）。
    void flushPending();

    // 删除流并清掉排队记录；调用方必须持锁。
    void removeStreamLocked(std::uint32_t streamId);

    // 流是否在排队中；调用方必须持锁。
    bool isPendingLocked(std::uint32_t streamId) const;

    // 锁外执行：先发帧，再回调事件。
    void dispatch(std::vector<protocol::PeerFrame> frames, const std::vector<Emit>& events);

    // 单个事件的便捷入口，内部转 dispatch。
    void emitEvent(std::uint32_t streamId, protocol::StreamEvent event, protocol::CloseReason reason);

    std::uint32_t peerNodeId_ = 0; // 所属对端
    StreamConfig config_;          // 流层参数
    FrameSender sender_;           // 注入的可靠发送
    bool connected_ = false;       // 由 onConnectionUp / onConnectionClosed 维护

    mutable std::mutex mutex_;                          // 保护流表、排队与两个 handler
    std::unordered_map<std::uint32_t, Stream> streams_; // 唯一流表
    std::deque<std::uint32_t> pending_;                 // 连接未就绪时排队的流
    std::size_t droppedFrames_ = 0;                     // 找不到流的 Data 帧计数

    StreamHandler streamHandler_; // 对外流事件，回调在锁外触发
    DataHandler dataHandler_;     // 对外数据，回调在锁外触发
};

} // namespace netproxy::transport
