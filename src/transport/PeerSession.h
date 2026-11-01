#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "net/Socket.h"
#include "protocol/Protocol.h"
#include "transport/connection/PeerConfig.h"
#include "transport/connection/PeerConnection.h"
#include "transport/connection/PeerProtocol.h"
#include "transport/connection/TransportTypes.h"
#include "transport/stream/StreamConfig.h"
#include "transport/stream/StreamMux.h"
#include "transport/stream/StreamTypes.h"

namespace netproxy::transport {

/**
 * 每对端一个会话：把「一条连接」与「其上的流」组合起来。
 *
 * 职责仅限于组装与转调：
 * - 连接层：握手、可靠帧通道、心跳、关闭
 * - 流层：流表、流信令、数据分发
 * - 状态同步：连接就绪后通知流层补发排队开流；连接关闭时广播流关闭
 *
 * 不持有流表与连接状态镜像：`connected_` 只用于跨线程查询。
 */
class PeerSession final {
public:
    /**
     * @param localNodeId 本节点标识。
     * @param peerNodeId 对端节点标识。
     * @param peer 对端地址。
     * @param peerConfig 连接参数。
     * @param streamConfig 流参数。
     * @param sender 报文发送函数。
     */
    PeerSession(std::uint32_t localNodeId, std::uint32_t peerNodeId, const net::IPAddress& peer,
                const PeerConfig& peerConfig, const StreamConfig& streamConfig, PacketSender sender);

    PeerSession(const PeerSession&) = delete;
    PeerSession& operator=(const PeerSession&) = delete;

    // 连接层转发。
    bool connect();
    void acceptHandshake();
    void onHelloAck(const HelloMessage& message);
    void onHelloReject(const HelloMessage& message);
    void onFrame(const protocol::PeerFrame& frame);
    void tick(std::chrono::steady_clock::time_point now);
    void close(protocol::Reason reason);

    // 流层转发。
    bool openStream(std::uint32_t streamId);
    bool sendData(std::uint32_t streamId, const void* data, std::size_t size);
    bool closeStream(std::uint32_t streamId);
    bool ownsStream(std::uint32_t streamId) const;
    bool isStreamOpen(std::uint32_t streamId) const;
    std::vector<StreamRecord> streams() const;

    void setStreamHandler(StreamMux::StreamHandler handler);
    void setDataHandler(StreamMux::DataHandler handler);

    // 查询：跨线程安全。
    std::uint32_t peerNodeId() const;
    const net::IPAddress& peer() const;
    bool isConnected() const;

    // 仅业务线程可读。
    ConnectionState state() const;

private:
    // 握手成功后推进流层状态：补发排队开流。
    void onHandshakeCompleted();

    PeerConnection connection_;
    StreamMux streams_;
    std::atomic<bool> connected_{false}; // 供应用线程查询
};

} // namespace netproxy::transport
