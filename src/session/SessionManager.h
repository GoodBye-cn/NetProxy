#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "DataChannel.h"
#include "PeerInfo.h"
#include "SessionProtocol.h"
#include "SessionTable.h"
#include "core/IoLoop.h"
#include "core/TaskLoop.h"

namespace netproxy::session {

// 会话模块配置。
struct SessionConfig {
    std::size_t maxSessions = 64;       // 总会话上限
    std::size_t maxSessionsPerPeer = 8; // 单对节点上限
    int openRetryIntervalMs = 1000;     // Open 重发间隔
    int openRetryCount = 5;             // Open 最多重发次数
    int closeTimeoutMs = 2000;          // Close 等待时间

    int keepaliveIntervalMs = 30000; // 空闲达到该时间后发送 Ping
    int keepaliveTimeoutMs = 10000;  // 每次 Ping 的等待时间
    int keepaliveRetryCount = 3;     // 连续无响应次数上限，超过判定断链
    int keepaliveTickMs = 1000;      // 保活检查粒度
};

/**
 * 会话管理器。
 *
 * 状态机运行在业务线程，会话报文通过 DataChannel 收发：
 * - I/O 线程读到报文后投递到业务线程；
 * - 业务线程推进状态机，发送走 DataChannel::send。
 *
 * 不持有 socket，也不依赖节点管理器：对端地址通过 PeerResolver 查询。
 */
class SessionManager final {
public:
    // 会话事件。
    enum class Event {
        Opened,   // 握手完成
        Rejected, // 被拒绝或握手超时
        Closed    // 已关闭
    };

    // 会话事件回调：会话标识、对端节点标识、事件类型。
    using Handler = std::function<void(std::uint32_t sessionId, std::uint32_t peerNodeId, Event event)>;

    // 查询对端地址，返回 false 表示节点未知。
    using PeerResolver = std::function<bool(std::uint32_t nodeId, netproxy::PeerInfo& peer)>;

    /**
     * 构造会话管理器，不注册任何回调。
     *
     * @param channel 数据通道，必须比本对象活得久且已 open。
     * @param resolver 对端查询回调，可在任意线程调用。
     * @param localNodeId 本节点标识，写入 Open 报文。
     * @param config 会话配置。
     * @param io I/O 线程循环，必须比本对象活得久。
     * @param business 业务线程循环，必须比本对象活得久。
     */
    SessionManager(DataChannel& channel, PeerResolver resolver, std::uint32_t localNodeId, const SessionConfig& config,
                   core::TaskLoop& business);

    // 析构时停止模块。
    ~SessionManager();

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    /**
     * 启动会话模块。
     *
     * 设置数据通道回调，需在 IoLoop 启动前调用。
     *
     * @return 启动成功返回 true；数据通道未打开或注册失败返回 false。
     */
    bool start();

    /**
     * 停止会话模块。
     *
     * 清空会话表，数据通道回调置空。
     *
     * @note 应在业务线程与 I/O 线程停止之后调用。
     */
    void stop();

    /**
     * 判断模块是否已启动。
     *
     * @return 已启动且未停止返回 true。
     */
    bool isRunning() const;

    /**
     * 主动向指定节点发起会话。
     *
     * 立即返回分配的会话标识，握手在业务线程异步进行，结果通过 Handler 通知。
     *
     * @param peerNodeId 对端节点标识。
     * @return 新会话标识；模块未启动、参数非法或无法分配标识时返回 0。
     * @note 返回非 0 不代表对端可达，对端未知或超限会回调 Rejected。
     *       回调可能在返回前触发，调用方应提前准备好状态收集。
     */
    std::uint32_t open(std::uint32_t peerNodeId);

    /**
     * 关闭会话。
     *
     * @param sessionId 会话标识。
     * @return 关闭请求已投递返回 true；模块未启动或参数非法返回 false。
     * @note 只表示请求已受理，会话不存在时不会产生事件。
     */
    bool close(std::uint32_t sessionId);

    /**
     * 判断会话是否已建立。
     *
     * @param sessionId 会话标识。
     * @return 状态为 Established 返回 true。
     */
    bool isEstablished(std::uint32_t sessionId) const;

    /**
     * 获取会话快照，线程安全。
     *
     * @return 当前所有会话记录。
     */
    std::vector<SessionRecord> list() const;

    /**
     * 设置会话事件回调。
     *
     * @param handler 回调对象，在业务线程执行；为空表示取消。
     */
    void setHandler(Handler handler);

private:
    // I/O 线程：数据通道回调。
    void handleDatagram(const net::IPAddress& from, const char* data, std::size_t size);

    // 业务线程：按类型分发报文。
    void onMessage(const net::IPAddress& from, const SessionMessage& message);

    // 调用线程：生成不与现有会话冲突的标识。
    std::uint32_t allocateSessionId();

    // 业务线程：建立与关闭的实际处理。
    void beginOpen(std::uint32_t sessionId, std::uint32_t peerNodeId);
    bool closeOnBusiness(std::uint32_t sessionId);

    // 业务线程：状态机处理。
    void handleOpen(const net::IPAddress& from, const SessionMessage& message);
    void handleAccept(const net::IPAddress& from, const SessionMessage& message);
    void handleReject(const SessionMessage& message);
    void handleClose(const SessionMessage& message);

    // 发送报文。
    void sendMessage(const SessionMessage& message, const net::IPAddress& target);

    // 握手重传。
    void scheduleRetry(std::uint32_t sessionId);
    void retry(std::uint32_t sessionId);

    // 保活：处理 Ping、刷新活动时间、周期检查与调度。
    void handlePing(const net::IPAddress& from, const SessionMessage& message);
    void touchSession(std::uint32_t sessionId);
    void tickKeepalive();
    void scheduleKeepalive();

    // 回复拒绝。
    void sendReject(std::uint32_t sessionId, RejectReason reason, const net::IPAddress& target);

    // 通知外部事件。
    void notify(std::uint32_t sessionId, std::uint32_t peerNodeId, Event event);

    SessionConfig config_;
    SessionTable table_;
    DataChannel& channel_;
    PeerResolver resolver_;
    std::uint32_t localNodeId_ = 0;
    core::TaskLoop& business_;
    mutable std::mutex handlerMutex_;
    Handler handler_;
    std::atomic<bool> running_{false};
};

} // namespace netproxy::session
