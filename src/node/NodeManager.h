#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "NodeDiscovery.h"
#include "PeerTable.h"
#include "core/IoLoop.h"
#include "core/TaskLoop.h"

namespace netproxy::node {

// 节点管理配置。
struct NodeConfig {
    int announceIntervalMs = 3000; // 公告周期
    int peerTimeoutMs = 10000;     // 节点过期时间
    int tickIntervalMs = 200;      // 定时检查粒度
};

/**
 * 节点管理器，运行在业务线程。
 *
 * 只负责状态：维护节点表、驱动周期公告、淘汰过期节点。
 * 组播 socket 由 NodeDiscovery 持有，公告报文通过回调推入。
 */
class NodeManager final {
public:
    // 发现新节点时的回调，运行在业务线程。
    using PeerHandler = std::function<void(const PeerInfo& peer)>;

    /**
     * 构造节点管理器。
     *
     * @param discovery 发现通道，必须比本对象活得久且已 open。
     * @param nodeId 本节点标识。
     * @param config 管理配置。
     * @param io I/O 线程循环，必须比本对象活得久。
     * @param business 业务线程循环，必须比本对象活得久。
     */
    NodeManager(NodeDiscovery& discovery, std::uint32_t nodeId, const NodeConfig& config, core::IoLoop& io,
                core::TaskLoop& business);

    // 析构时停止定时任务。
    ~NodeManager();

    NodeManager(const NodeManager&) = delete;
    NodeManager& operator=(const NodeManager&) = delete;

    /**
     * 启动节点管理。
     *
     * 设置发现通道回调并安排第一个 tick，需在 IoLoop / TaskLoop 启动前调用。
     *
     * @return 设置回调成功返回 true；发现通道未打开或注册失败返回 false。
     */
    bool start();

    // 停止定时任务，幂等。
    void stop();

    /**
     * 获取本节点标识。
     *
     * @return 构造时传入的 nodeId。
     */
    std::uint32_t nodeId() const;

    /**
     * 获取已发现节点快照，线程安全。
     *
     * @return 当前所有已发现节点。
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
     * 设置新节点回调。
     *
     * @param handler 回调对象，为空表示取消；运行中替换会加锁。
     */
    void setPeerHandler(PeerHandler handler);

    /**
     * 记录一个探测到的节点。
     *
     * @param peer 节点信息。
     * @note 由业务线程调用；发现通道的回调会先投递到这里。
     */
    void onPeerAnnounced(const PeerInfo& peer);

private:
    // 业务线程：周期公告与过期淘汰。
    void tick();

    // 业务线程：安排下一次 tick。
    void scheduleTick();

    NodeConfig config_;
    NodeDiscovery& discovery_;
    std::uint32_t nodeId_ = 0;
    PeerTable peers_;
    core::IoLoop& io_;
    core::TaskLoop& business_;
    mutable std::mutex handlerMutex_;
    PeerHandler handler_;
    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point lastAnnounce_{};
};

} // namespace netproxy::node
