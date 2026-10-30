#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "DataChannel.h"
#include "PeerConnection.h"
#include "core/TaskLoop.h"
#include "net/Socket.h"
#include "protocol/Protocol.h"

namespace netproxy::transport {

/**
 * 按对端节点管理连接。
 *
 * 数据 socket 的唯一入口：握手报文创建连接，连接帧分发到对应连接；
 * 同时负责「连接未就绪时排队开流」与事件转发。
 */
// 流的对外快照。
struct StreamRecord {
    std::uint32_t streamId = 0;
    std::uint32_t peerNodeId = 0;
    bool open = false;
};

class PeerConnectionManager final {
public:
    // 流事件：补充对端节点标识后转发给上层。
    using StreamHandler = std::function<void(std::uint32_t streamId, std::uint32_t peerNodeId,
                                             protocol::StreamEvent event, protocol::Reason reason)>;

    // 数据交付：补充对端节点标识后转发给上层。
    using DataHandler =
        std::function<void(std::uint32_t streamId, std::uint32_t peerNodeId, const char* data, std::size_t size)>;

    /**
     * 构造管理器, 不发送报文。
     *
     * @param channel 数据 socket。
     * @param localNodeId 本节点标识。
     * @param config 连接参数。
     * @param business 业务线程循环。
     */
    PeerConnectionManager(DataChannel& channel, std::uint32_t localNodeId, const PeerConfig& config,
                          core::TaskLoop& business);

    ~PeerConnectionManager();

    PeerConnectionManager(const PeerConnectionManager&) = delete;
    PeerConnectionManager& operator=(const PeerConnectionManager&) = delete;

    /**
     * 设置数据通道回调并启动定时检查，需在 IoLoop 启动前调用。
     *
     * @return 注册成功返回 true。
     */
    bool start();

    void stop();

    /**
     * 发起连接，握手在业务线程异步进行。
     *
     * @param peerNodeId 对端节点标识。
     * @param peer 对端地址。
     * @return 请求已投递返回 true。
     */
    bool connect(std::uint32_t peerNodeId, const net::IPAddress& peer);

    /**
     * 开一条流，连接未就绪时自动建连并排队。
     *
     * @param peerNodeId 对端节点标识。
     * @param peer 对端地址。
     * @return 分配的流标识；无法分配返回 0。
     */
    std::uint32_t openStream(std::uint32_t peerNodeId, const net::IPAddress& peer);

    /**
     * 关闭流。
     *
     * @param streamId 流标识。
     * @return 请求已投递返回 true。
     */
    bool closeStream(std::uint32_t streamId);

    /**
     * 向流提交数据。
     *
     * @param streamId 流标识。
     * @param data 数据内容。
     * @param size 长度。
     * @return 请求已投递返回 true。
     */
    bool sendData(std::uint32_t streamId, const void* data, std::size_t size);

    /**
     * 判断连接是否已建立。
     *
     * @param peerNodeId 对端节点标识。
     * @return 已连接返回 true。
     */
    bool isConnected(std::uint32_t peerNodeId) const;

    /**
     * 判断流是否已打开。
     *
     * @param streamId 流标识。
     * @return 已 Open 返回 true。
     */
    bool isStreamOpen(std::uint32_t streamId) const;

    /**
     * 获取流快照，线程安全。
     *
     * @return 当前所有流。
     */
    std::vector<StreamRecord> streams() const;

    void setStreamHandler(StreamHandler handler);
    void setDataHandler(DataHandler handler);

private:
    // 业务线程：解析报文并分发。
    void onDatagram(const net::IPAddress& from, const char* data, std::size_t size);

    // 业务线程：周期检查。
    void tick();
    void scheduleTick();

    // 查找或创建连接。
    std::shared_ptr<PeerConnection> ensureConnection(std::uint32_t peerNodeId, const net::IPAddress& peer);

    // 连接就绪后补发排队的流。
    void flushPending(std::uint32_t peerNodeId);

    // 转发流事件与数据。
    void emitStreamEvent(std::uint32_t streamId, std::uint32_t peerNodeId, protocol::StreamEvent event,
                         protocol::Reason reason);
    void emitData(std::uint32_t streamId, std::uint32_t peerNodeId, const char* data, std::size_t size);

    // 生成非 0 流标识。
    std::uint32_t allocateStreamId();

    // 查询流所属连接，加锁。
    std::uint32_t peerForStream(std::uint32_t streamId) const;

    // 从流表移除，加锁。
    void removeStream(std::uint32_t streamId);

    DataChannel& channel_;
    std::uint32_t localNodeId_ = 0;
    PeerConfig config_;
    core::TaskLoop& business_;
    PacketSender sender_;

    std::unordered_map<std::uint32_t, std::shared_ptr<PeerConnection>> connections_;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> pendingStreams_;

    mutable std::mutex stateMutex_;
    std::unordered_map<std::uint32_t, bool> connectedCache_;
    std::unordered_map<std::uint32_t, std::uint32_t> streamToPeer_;
    std::unordered_map<std::uint32_t, bool> streamOpen_;

    StreamHandler streamHandler_;
    DataHandler dataHandler_;
    std::atomic<bool> running_{false};
};

} // namespace netproxy::transport
