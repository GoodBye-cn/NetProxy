#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "PeerInfo.h"

namespace netproxy::node {

// 已发现节点的记录，附带最近一次收到公告的时间。
struct PeerRecord {
    PeerInfo info;                                  // 对端标识、地址与端口
    std::chrono::steady_clock::time_point lastSeen; // 最近一次收到公告的时间
};

/**
 * 已发现节点表，线程安全。
 *
 * 探测线程写入与刷新，业务线程可随时查询。
 * 节点是否有连接由会话层决定，本类只记录探测结果。
 */
class PeerTable final {
public:
    /**
     * 新增或刷新节点。
     *
     * 已存在的 nodeId 会更新地址与端口，并刷新最近可见时间。
     *
     * @param peer 探测到的节点信息。
     * @return 首次记录该节点返回 true，刷新已有节点返回 false。
     */
    bool update(const PeerInfo& peer);

    /**
     * 移除指定节点。
     *
     * @param nodeId 节点标识。
     * @return 存在并移除成功返回 true。
     */
    bool remove(std::uint32_t nodeId);

    /**
     * 判断节点是否存在。
     *
     * @param nodeId 节点标识。
     * @return 存在返回 true。
     */
    bool contains(std::uint32_t nodeId) const;

    /**
     * 查询节点信息。
     *
     * @param nodeId 节点标识。
     * @param peer 输出节点信息，仅在返回 true 时被写入。
     * @return 找到返回 true，否则返回 false。
     */
    bool find(std::uint32_t nodeId, PeerInfo& peer) const;

    /**
     * 获取全部节点快照。
     *
     * @return 当前所有已发现节点的副本。
     */
    std::vector<PeerInfo> list() const;

    /**
     * 淘汰长时间未刷新公告的节点。
     *
     * @param timeout 允许的最大静默时长。
     * @return 被淘汰的节点数量。
     */
    std::size_t removeExpired(std::chrono::milliseconds timeout);

    /**
     * 获取节点数量。
     *
     * @return 当前表中的节点数。
     */
    std::size_t size() const;

    // 清空所有节点。
    void clear();

private:
    mutable std::mutex mutex_;                            // 保护 peers_
    std::unordered_map<std::uint32_t, PeerRecord> peers_; // 对端标识到记录的映射
};

} // namespace netproxy::node
