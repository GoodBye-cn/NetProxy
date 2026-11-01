#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/TaskLoop.h"
#include "net/Socket.h"
#include "transport/DataChannel.h"
#include "transport/PeerSession.h"
#include "transport/connection/PeerConfig.h"
#include "transport/connection/TransportTypes.h"
#include "transport/stream/StreamConfig.h"
#include "transport/stream/StreamMux.h"
#include "transport/stream/StreamTypes.h"

namespace netproxy::transport {

/**
 * 按对端节点路由报文与会话。
 *
 * 职责：
 * - 数据 socket 的唯一入口：握手报文建会话、连接帧分发到对应会话
 * - `nodeId → PeerSession` 表与统一 tick
 * - 对外 API 转发
 *
 * 不保存任何流状态：流表在 `StreamMux`，连接状态在 `PeerSession`。
 */
class PeerSessionManager final {
public:
    /**
     * @param channel 数据 socket。
     * @param localNodeId 本节点标识。
     * @param peerConfig 连接参数。
     * @param streamConfig 流参数。
     * @param business 业务线程循环。
     */
    PeerSessionManager(DataChannel& channel, std::uint32_t localNodeId, const PeerConfig& peerConfig,
                          const StreamConfig& streamConfig, core::TaskLoop& business);

    ~PeerSessionManager();

    PeerSessionManager(const PeerSessionManager&) = delete;
    PeerSessionManager& operator=(const PeerSessionManager&) = delete;

    // 设置数据通道回调并启动定时检查，需在 IoLoop 启动前调用。
    bool start();

    void stop();

    // 发起连接，握手在业务线程异步进行。
    bool connect(std::uint32_t peerNodeId, const net::IPAddress& peer);

    // 开一条流；连接未就绪时自动建连并排队。
    std::uint32_t openStream(std::uint32_t peerNodeId, const net::IPAddress& peer);

    // 关闭流、发送数据；均在业务线程执行。
    bool closeStream(std::uint32_t streamId);
    bool sendData(std::uint32_t streamId, const void* data, std::size_t size);

    bool isConnected(std::uint32_t peerNodeId) const;
    bool isStreamOpen(std::uint32_t streamId) const;
    std::vector<StreamRecord> streams() const;

    void setStreamHandler(StreamMux::StreamHandler handler);
    void setDataHandler(StreamMux::DataHandler handler);

private:
    // 业务线程：解析报文并分发。
    void onDatagram(const net::IPAddress& from, const char* data, std::size_t size);

    // 业务线程：周期检查。
    void tick();
    void scheduleTick();

    // 查找或创建会话，并接好事件转发。
    std::shared_ptr<PeerSession> ensureSession(std::uint32_t peerNodeId, const net::IPAddress& peer);

    // 按流号定位会话；对端数量为个位数，线性扫描。
    std::shared_ptr<PeerSession> sessionForStream(std::uint32_t streamId) const;

    // 取会话快照，避免持锁调用会话方法。
    std::vector<std::shared_ptr<PeerSession>> snapshotSessions() const;

    // 生成非 0 流标识。
    std::uint32_t allocateStreamId();

    // 转发流事件与数据。
    void emitStreamEvent(std::uint32_t streamId, std::uint32_t peerNodeId, protocol::StreamEvent event,
                         protocol::Reason reason);
    void emitData(std::uint32_t streamId, std::uint32_t peerNodeId, const char* data, std::size_t size);

    DataChannel& channel_;
    std::uint32_t localNodeId_ = 0;
    PeerConfig peerConfig_;
    StreamConfig streamConfig_;
    core::TaskLoop& business_;
    PacketSender sender_;

    mutable std::mutex sessionsMutex_;
    std::unordered_map<std::uint32_t, std::shared_ptr<PeerSession>> sessions_;
    std::atomic<bool> running_{false};

    StreamMux::StreamHandler streamHandler_;
    StreamMux::DataHandler dataHandler_;
};

} // namespace netproxy::transport
