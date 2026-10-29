#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "PeerInfo.h"

namespace netproxy {

// 会话事件。
enum class SessionEvent {
    Opened,   // 握手完成
    Rejected, // 被对端拒绝或握手超时
    Closed    // 会话已关闭
};

// 会话快照。
struct SessionInfo {
    std::uint32_t sessionId = 0;
    std::uint32_t peerNodeId = 0;
    bool established = false; // 握手是否已完成
};

// P2P 服务配置。
struct P2PServiceConfig {
    std::uint32_t nodeId = 0;    // 0 表示启动时随机生成
    std::uint16_t dataPort = 0;  // 0 表示由系统分配
    std::string dataBindAddress; // 空串表示 0.0.0.0

    std::string multicastGroup = "239.255.42.99"; // 发现通道组播组
    std::uint16_t multicastPort = 45454;          // 发现通道组播端口
    int announceIntervalMs = 3000;                // 公告周期
    int peerTimeoutMs = 10000;                    // 节点过期时间

    std::size_t maxSessions = 64;       // 会话总数上限
    std::size_t maxSessionsPerPeer = 8; // 单对节点会话上限
    int sessionRetryIntervalMs = 1000;  // Open 重发间隔
    int sessionRetryCount = 5;          // Open 最多重发次数
};

/**
 * 局域网 P2P 节点服务。
 *
 * 内部包含一个 I/O 线程与一个业务线程：
 * - I/O 线程负责数据 socket 与组播 socket 的读写；
 * - 业务线程负责节点表、会话状态机、周期公告与过期淘汰。
 *
 * 通过 pimpl 隐藏内部实现，外部只需包含本头文件。
 */
class P2PService final {
public:
    /**
     * 发现新节点时的回调。
     *
     * @param peer 对端地址、数据端口与节点标识。
     * @note 回调运行在业务线程，不能执行阻塞操作，也不要调用 stop()。
     */
    using PeerHandler = std::function<void(const PeerInfo& peer)>;

    /**
     * 会话事件回调。
     *
     * @param sessionId 会话标识。
     * @param peerNodeId 对端节点标识。
     * @param event 事件类型。
     * @note 回调运行在业务线程，不能执行阻塞操作，也不要调用 stop()。
     */
    using SessionHandler = std::function<void(std::uint32_t sessionId, std::uint32_t peerNodeId, SessionEvent event)>;

    // 构造服务，不启动线程。
    P2PService();

    // 析构时自动停止。
    ~P2PService();

    P2PService(const P2PService&) = delete;
    P2PService& operator=(const P2PService&) = delete;

    /**
     * 启动服务。
     *
     * 顺序：绑定数据 socket -> 打开组播服务 -> 注册会话 socket ->
     * 启动 I/O 线程 -> 启动业务线程。
     *
     * @param config 服务配置，未配置项使用默认值。
     * @return 启动成功返回 true；任一步失败返回 false 并回滚。
     * @note 重复调用安全，已启动时直接返回 true；不应与 stop() 并发调用。
     */
    bool start(const P2PServiceConfig& config);

    /**
     * 停止服务并等待两个线程退出。
     *
     * @note 幂等；不能在回调或工作线程中调用。
     */
    void stop();

    /**
     * 判断服务是否在运行。
     *
     * @return 已启动且未停止返回 true。
     */
    bool isRunning() const;

    /**
     * 获取实际生效的节点标识。
     *
     * @return 配置非 0 时与其一致，配置为 0 时为随机生成值；未启动返回 0。
     */
    std::uint32_t nodeId() const;

    /**
     * 获取实际数据端口。
     *
     * @return 配置固定端口时与其一致，配置为 0 时为系统分配值；未启动返回 0。
     */
    std::uint16_t dataPort() const;

    /**
     * 获取已发现节点快照，线程安全。
     *
     * @return 当前所有已发现节点；未启动返回空列表。
     */
    std::vector<PeerInfo> peers() const;

    /**
     * 查询指定节点，线程安全。
     *
     * @param nodeId 节点标识。
     * @param peer 输出节点信息，仅在返回 true 时被写入。
     * @return 已发现返回 true。
     */
    bool findPeer(std::uint32_t nodeId, PeerInfo& peer) const;

    /**
     * 设置发现新节点时的回调。
     *
     * @param handler 回调对象，为空表示取消。
     * @note 可在启动前或运行中设置；运行中替换时内部加锁保护。
     */
    void setPeerHandler(PeerHandler handler);

    /**
     * 向指定节点发起会话。
     *
     * 立即返回分配的会话标识，握手在业务线程异步进行，结果通过 SessionHandler 通知。
     *
     * @param peerNodeId 对端节点标识。
     * @return 新会话标识；服务未启动、参数非法或无法分配标识时返回 0。
     * @note 返回非 0 不代表对端可达：对端未知、超出上限或对端拒绝都会以 Rejected 回调。
     *       回调可能在返回前触发，调用方应提前准备好状态收集。
     */
    std::uint32_t openSession(std::uint32_t peerNodeId);

    /**
     * 关闭会话。
     *
     * @param sessionId 会话标识。
     * @return 关闭请求已投递返回 true；服务未启动或参数非法返回 false。
     * @note 只表示请求已受理，会话不存在时不会产生事件。
     */
    bool closeSession(std::uint32_t sessionId);

    /**
     * 判断会话是否已建立。
     *
     * @param sessionId 会话标识。
     * @return 状态为已建立返回 true。
     */
    bool isSessionEstablished(std::uint32_t sessionId) const;

    /**
     * 获取当前会话快照，线程安全。
     *
     * @return 所有会话，包含握手中的记录；服务未启动返回空列表。
     */
    std::vector<SessionInfo> sessions() const;

    /**
     * 设置会话事件回调。
     *
     * @param handler 回调对象，为空表示取消。
     * @note 可在启动前或运行中设置。
     */
    void setSessionHandler(SessionHandler handler);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace netproxy
