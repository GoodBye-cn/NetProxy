#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
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
 * - `peerNodeId → PeerSession` 表与统一 tick
 * - 对外 API 转发
 *
 * 不保存任何流状态：流表在 `StreamMux`，连接状态在 `PeerSession`。
 *
 * 线程模型：报文处理与状态写入都在业务线程；`isConnected` / `isStreamOpen` / `streams`
 * 与两个 `setXxxHandler` 可在任意线程调用，内部加锁或只读原子量。
 */
class PeerSessionManager final {
public:
    /**
     * 构造管理器，不发送报文。
     *
     * @param channel 数据 socket，必须比管理器活得久。
     * @param localNodeId 本节点标识，写入每条报文头部。
     * @param peerConfig 连接参数，构造时拷贝。
     * @param streamConfig 流参数，构造时拷贝。
     * @param business 业务线程循环，必须比管理器活得久。
     * @note 构造不注册回调也不定时；需调用 start() 后才开始收包与 tick。
     */
    PeerSessionManager(DataChannel& channel, std::uint32_t localNodeId, const PeerConfig& peerConfig,
                       const StreamConfig& streamConfig, core::TaskLoop& business);

    // 停止并释放全部会话；不向对端发送 Close。
    ~PeerSessionManager();

    PeerSessionManager(const PeerSessionManager&) = delete;
    PeerSessionManager& operator=(const PeerSessionManager&) = delete;

    /**
     * 注册数据通道回调并启动定时检查。
     *
     * @return 注册成功返回 true；已启动或通道注册失败返回 false。
     * @note 需在 IoLoop 启动前调用；重复调用不会重复注册。
     */
    bool start();

    /**
     * 注销数据通道回调并清空会话表。
     *
     * @note 重复调用无副作用；不向对端发送 Close。
     */
    void stop();

    /**
     * 发起连接，握手在业务线程异步进行。
     *
     * @param peerNodeId 对端节点标识。
     * @param peer 对端地址与端口。
     * @return 已投递到业务线程返回 true；未启动返回 false。
     * @note 返回 true 只表示已入队，不代表握手成功。
     */
    bool connect(std::uint32_t peerNodeId, const net::IPAddress& peer);

    /**
     * 开一条流；连接未就绪时自动建连并排队。
     *
     * @param peerNodeId 对端节点标识。
     * @param peer 对端地址，用于创建会话。
     * @return 分配的流号，调用线程立即拿到；未启动返回 0。
     * @note 流号在调用线程生成，建流在业务线程执行。
     */
    std::uint32_t openStream(std::uint32_t peerNodeId, const net::IPAddress& peer);

    /**
     * 关闭一条流。
     *
     * @param streamId 流标识。
     * @return 已投递到业务线程返回 true；未启动返回 false。
     * @note 返回 true 只表示已入队，流可能已不存在。
     */
    bool closeStream(std::uint32_t streamId);

    /**
     * 向指定流提交数据。
     *
     * @param streamId 流标识。
     * @param data 数据首地址，调用返回后可释放。
     * @param size 长度。
     * @return 已投递到业务线程返回 true；未启动或参数非法返回 false。
     * @note 返回 true 只表示已入队；实际发送结果由流层与连接层判定。
     */
    bool sendData(std::uint32_t streamId, const void* data, std::size_t size);

    /**
     * 判断与指定对端的连接是否已建立。
     *
     * @param peerNodeId 对端节点标识。
     * @return 会话存在且处于 Connected 返回 true。
     * @note 读会话的原子标志，任意线程可调用。
     */
    bool isConnected(std::uint32_t peerNodeId) const;

    /**
     * 判断流是否已打开。
     *
     * @param streamId 流标识。
     * @return 流存在且状态为 Open 返回 true。
     * @note 线性扫描各会话，任意线程可调用。
     */
    bool isStreamOpen(std::uint32_t streamId) const;

    /**
     * 获取所有会话的流快照。
     *
     * @return 当前全部流；无会话或无线程安全问题时返回空。
     * @note 先复制会话指针再逐个取快照，不持会话锁调用。
     */
    std::vector<StreamRecord> streams() const;

    /**
     * 注册流事件回调。
     *
     * @param handler 回调对象；为空表示不通知。
     * @note 可在任意线程调用；回调在业务线程触发。
     */
    void setStreamHandler(StreamMux::StreamHandler handler);

    /**
     * 注册数据回调。
     *
     * @param handler 回调对象；为空表示丢弃数据。
     * @note 可在任意线程调用；回调在业务线程触发。
     */
    void setDataHandler(StreamMux::DataHandler handler);

private:
    // I/O 线程：拷贝报文后投递到业务线程。
    void onDatagramReceived(const net::IPAddress& from, const char* data, std::size_t size);

    // 业务线程：解析报文并分发到会话。
    void onDatagram(const net::IPAddress& from, const char* data, std::size_t size);

    // 业务线程：持报文副本的入口，供投递任务绑定。
    void dispatchDatagram(const net::IPAddress& from, const std::string& packet);

    // 业务线程：发起握手。
    void connectSession(std::uint32_t peerNodeId, const net::IPAddress& peer);

    // 业务线程：建流，连接未就绪时同时发起握手。
    void openStreamOnSession(std::uint32_t peerNodeId, const net::IPAddress& peer, std::uint32_t streamId);

    // 业务线程：关闭流。
    void closeStreamOnSession(std::uint32_t streamId);

    // 业务线程：向指定流提交数据。
    void sendDataOnStream(std::uint32_t streamId, const std::string& payload);

    // 业务线程：遍历会话 tick 并清理已关闭的会话。
    void tick();

    // 安排下一次 tick；未运行时不再续约。
    void scheduleTick();

    // 查找或创建会话，并接好事件转发。
    std::shared_ptr<PeerSession> ensureSession(std::uint32_t peerNodeId, const net::IPAddress& peer);

    // 按流号定位会话；对端数量为个位数，线性扫描。
    std::shared_ptr<PeerSession> sessionForStream(std::uint32_t streamId) const;

    // 取会话快照，避免持锁调用会话方法。
    std::vector<std::shared_ptr<PeerSession>> snapshotSessions() const;

    // 生成非 0 流标识。
    std::uint32_t allocateStreamId();

    // 转发流事件给上层；锁内只复制回调，锁外调用。
    void emitStreamEvent(std::uint32_t streamId, std::uint32_t peerNodeId, protocol::StreamEvent event,
                         protocol::CloseReason reason);

    // 转发数据给上层；锁内只复制回调，锁外调用。
    void emitData(std::uint32_t streamId, std::uint32_t peerNodeId, const char* data, std::size_t size);

    DataChannel& channel_;          // 数据 socket
    std::uint32_t localNodeId_ = 0; // 本节点标识
    PeerConfig peerConfig_;         // 连接参数
    StreamConfig streamConfig_;     // 流参数
    core::TaskLoop& business_;      // 业务线程循环
    PacketSender sender_;           // 报文发送函数，注入给会话

    mutable std::mutex sessionsMutex_;                                         // 保护 sessions_ 与两个 handler
    std::unordered_map<std::uint32_t, std::shared_ptr<PeerSession>> sessions_; // 对端到会话
    std::atomic<bool> running_{false};                                         // 是否已 start 且未 stop

    StreamMux::StreamHandler streamHandler_; // 对外流事件
    StreamMux::DataHandler dataHandler_;     // 对外数据
};

} // namespace netproxy::transport
