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
 *
 * 线程模型：除 `isConnected()` / `peerNodeId()` / `peer()` 外，其余方法只允许在业务线程调用。
 */
class PeerSession final {
public:
    /**
     * 构造会话，不发送任何报文。
     *
     * @param localNodeId 本节点标识。
     * @param peerNodeId 对端节点标识。
     * @param peer 对端地址与端口。
     * @param peerConfig 连接参数，构造时拷贝。
     * @param streamConfig 流参数，构造时拷贝。
     * @param sender 报文发送函数，通常转 `DataChannel::send`。
     * @note 构造后连接处于 Connecting；需调用 connect() 或 acceptHandshake() 推进状态。
     */
    PeerSession(std::uint32_t localNodeId, std::uint32_t peerNodeId, const net::IPAddress& peer,
                const PeerConfig& peerConfig, const StreamConfig& streamConfig, PacketSender sender);

    PeerSession(const PeerSession&) = delete;
    PeerSession& operator=(const PeerSession&) = delete;

    /**
     * 发起握手：生成 nonce 并发送 Hello。
     *
     * @return 已发出首个 Hello 返回 true；已发起过或状态不允许返回 false。
     */
    bool connect();

    /**
     * 作为接收方确认握手，进入 Connected。
     *
     * 随后调用 `StreamMux::onConnectionUp()` 补发排队中的开流。
     */
    void acceptHandshake();

    /**
     * 处理 HelloAck。
     *
     * @param message 已解析的握手报文；nonce 不匹配时忽略。
     * @note 校验通过后进入 Connected，并补发排队开流。
     */
    void onHelloAck(const HelloMessage& message);

    /**
     * 处理 HelloReject。
     *
     * @param message 已解析的拒绝报文，reason 仅用于日志。
     * @note 连接进入 Closed，流层随之广播流关闭。
     */
    void onHelloReject(const HelloMessage& message);

    /**
     * 处理一条连接帧。
     *
     * @param frame 已解析的帧；序号、确认、重传与有序交付都在连接层完成。
     */
    void onFrame(const protocol::PeerFrame& frame);

    /**
     * 周期检查。
     *
     * @param now 当前时间，由 manager 统一传入，便于测试注入。
     * @note 触发握手重发、重传、心跳、空闲与关闭超时。
     */
    void tick(std::chrono::steady_clock::time_point now);

    /**
     * 关闭连接并通知流层。
     *
     * @param reason 关闭原因，透传给流事件。
     */
    void close(protocol::CloseReason reason);

    /**
     * 建流并发送 StreamOpen。
     *
     * @param streamId 流标识，由调用线程生成，非 0。
     * @return 已建流返回 true；重复流号、超上限或发送失败返回 false。
     * @note 连接未就绪时流仍建表并排队，握手完成后补发。
     */
    bool openStream(std::uint32_t streamId);

    /**
     * 向指定流提交数据。
     *
     * @param streamId 流标识。
     * @param data 数据首地址，调用返回后可释放。
     * @param size 长度，1~maxPayloadSize。
     * @return 连接层已受理返回 true；流未 Open、连接不可用或超长返回 false。
     * @note 返回 true 只表示本地受理，不代表对端收到。
     */
    bool sendData(std::uint32_t streamId, const void* data, std::size_t size);

    /**
     * 关闭一条流。
     *
     * @param streamId 流标识。
     * @return 流存在并已处理返回 true；流不存在返回 false。
     * @note 排队中的流只回调 `Closed(Local)`，不向对端发送 StreamClose。
     */
    bool closeStream(std::uint32_t streamId);

    /**
     * 判断流是否属于本会话。
     *
     * @param streamId 流标识。
     * @return 流在本会话流表中返回 true。
     */
    bool ownsStream(std::uint32_t streamId) const;

    /**
     * 判断流是否已打开。
     *
     * @param streamId 流标识。
     * @return 流存在且状态为 Open 返回 true。
     */
    bool isStreamOpen(std::uint32_t streamId) const;

    /**
     * 获取本会话全部流。
     *
     * @return 流快照，可跨线程调用。
     */
    std::vector<StreamRecord> streams() const;

    /**
     * 注册流事件回调。
     *
     * @param handler 回调对象，由 manager 在创建会话时设置。
     */
    void setStreamHandler(StreamMux::StreamHandler handler);

    /**
     * 注册数据回调。
     *
     * @param handler 回调对象，由 manager 在创建会话时设置。
     */
    void setDataHandler(StreamMux::DataHandler handler);

    /**
     * 获取对端节点标识。
     *
     * @return 构造时传入的 peerNodeId。
     */
    std::uint32_t peerNodeId() const;

    /**
     * 获取对端地址。
     *
     * @return 对端地址引用，构造后不变。
     */
    const net::IPAddress& peer() const;

    /**
     * 判断连接是否已建立。
     *
     * @return Connected 返回 true；读 atomic，任意线程可调用。
     */
    bool isConnected() const;

    /**
     * 获取连接状态。
     *
     * @return 当前 ConnectionState。
     * @note 仅业务线程可读。
     */
    ConnectionState state() const;

private:
    // 供连接层判定空闲关闭；流表非空返回 true。
    bool hasStreams() const;

    // 连接层进入 Closed：置跨线程标志并广播流关闭。
    void onConnectionClosed(protocol::CloseReason reason);

    // 握手成功后推进流层状态：置 connected_ 并补发排队开流。
    void onHandshakeCompleted();

    PeerConnection connection_;          // 连接层：握手、可靠帧通道、心跳
    StreamMux streams_;                  // 流层：流表与流信令
    std::atomic<bool> connected_{false}; // 供应用线程查询，避免读连接层状态
};

} // namespace netproxy::transport
