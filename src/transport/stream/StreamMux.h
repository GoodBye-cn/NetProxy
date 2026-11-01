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
                                             protocol::StreamEvent event, protocol::Reason reason)>;

    // 数据交付。
    using DataHandler =
        std::function<void(std::uint32_t streamId, std::uint32_t peerNodeId, const char* data, std::size_t size)>;

    // 注入连接层的可靠发送；返回 false 表示连接不可用或负载超限。
    using FrameSender = std::function<bool(protocol::PeerFrame)>;

    /**
     * @param peerNodeId 所属对端。
     * @param config 流层参数，构造时拷贝。
     * @param sender 可靠发送函数。
     */
    StreamMux(std::uint32_t peerNodeId, const StreamConfig& config, FrameSender sender);

    StreamMux(const StreamMux&) = delete;
    StreamMux& operator=(const StreamMux&) = delete;

    // 连接层驱动的入口（业务线程）。
    void onFrame(const protocol::PeerFrame& frame);             // 只处理 Data / Stream*
    void onConnectionUp();                            // 握手完成：补发排队开流
    void onConnectionClosed(protocol::Reason reason); // 已开流 Closed、排队流 Rejected

    // 上层 API（业务线程）。
    // 流号由调用线程生成后传入，Mux 只负责建流与排队。
    bool openStream(std::uint32_t streamId);
    bool sendData(std::uint32_t streamId, const void* data, std::size_t size);
    bool closeStream(std::uint32_t streamId);

    // 任意线程查询（内部加锁）。
    bool hasStreams() const;
    bool isStreamOpen(std::uint32_t streamId) const;
    bool ownsStream(std::uint32_t streamId) const;
    std::vector<StreamRecord> streams() const;

    void setStreamHandler(StreamHandler handler);
    void setDataHandler(DataHandler handler);

private:
    // 锁外触发的流事件。
    struct Emit {
        std::uint32_t streamId = 0;
        protocol::StreamEvent event = protocol::StreamEvent::Opened;
        protocol::Reason reason = static_cast<protocol::Reason>(0);
    };

    void handleStreamOpen(const protocol::PeerFrame& frame);
    void handleStreamAccept(const protocol::PeerFrame& frame);
    void handleStreamReject(const protocol::PeerFrame& frame);
    void handleStreamClose(const protocol::PeerFrame& frame);
    void handleData(const protocol::PeerFrame& frame);

    // 发送 StreamOpen；失败时删流并回调 Rejected(Error)。
    bool sendStreamOpen(std::uint32_t streamId);

    // 把 pending_ 中的流全部补发（onConnectionUp 内部使用）。
    void flushPending();

    // 以下两个由调用方持锁调用。
    void removeStreamLocked(std::uint32_t streamId);
    bool isPendingLocked(std::uint32_t streamId) const;

    // 锁外执行：先发帧，再回调事件。
    void dispatch(std::vector<protocol::PeerFrame> frames, const std::vector<Emit>& events);
    void emitEvent(std::uint32_t streamId, protocol::StreamEvent event, protocol::Reason reason);

    std::uint32_t peerNodeId_ = 0;
    StreamConfig config_;
    FrameSender sender_;
    bool connected_ = false; // 由 onConnectionUp / onConnectionClosed 维护

    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, Stream> streams_; // 唯一流表
    std::deque<std::uint32_t> pending_;                 // 连接未就绪时排队的流
    std::size_t droppedFrames_ = 0;                     // 找不到流的 Data 帧计数

    StreamHandler streamHandler_;
    DataHandler dataHandler_;
};

} // namespace netproxy::transport
