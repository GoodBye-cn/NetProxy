#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "net/Socket.h"

namespace netproxy::session {

// 会话状态。
enum class SessionState : std::uint8_t {
    Opening = 1,     // 已发 Open，等待响应
    Established = 2, // 握手完成
    Closing = 3,     // 已发 Close
    Closed = 4       // 终止，等待移除
};

// 单条会话的记录。
struct SessionRecord {
    std::uint32_t sessionId = 0;
    std::uint32_t peerNodeId = 0;
    net::IPAddress peerAddress; // 对端地址，含端口
    SessionState state = SessionState::Opening;
    std::uint32_t nonce = 0;
    int retryCount = 0;
    std::chrono::steady_clock::time_point deadline{}; // 下一次超时检查时间

    // 保活状态，仅 Established 有意义
    std::chrono::steady_clock::time_point lastActivity{}; // 最近一次收到对端报文
    std::chrono::steady_clock::time_point pingSentAt{};   // 最近一次发送 Ping
    int keepaliveMisses = 0;                              // 连续未响应次数
};

/**
 * 会话表，线程安全。
 *
 * 状态机只在业务线程推进，查询接口可被其它线程调用。
 */
class SessionTable final {
public:
    /**
     * 插入会话，sessionId 已存在时失败。
     *
     * @param record 会话记录。
     * @return 插入成功返回 true。
     */
    bool insert(const SessionRecord& record);

    /**
     * 查询会话。
     *
     * @param sessionId 会话标识。
     * @param record 输出记录，仅在返回 true 时被写入。
     * @return 存在返回 true。
     */
    bool find(std::uint32_t sessionId, SessionRecord& record) const;

    /**
     * 更新已存在的会话。
     *
     * @param record 新的记录内容。
     * @return 会话存在并更新成功返回 true。
     */
    bool update(const SessionRecord& record);

    /**
     * 移除会话。
     *
     * @param sessionId 会话标识。
     * @return 存在并移除成功返回 true。
     */
    bool remove(std::uint32_t sessionId);

    /**
     * 获取会话总数。
     *
     * @return 当前记录数。
     */
    std::size_t count() const;

    /**
     * 获取指定对端的会话数。
     *
     * @param peerNodeId 对端节点标识。
     * @return 该对端的会话数量。
     */
    std::size_t countForPeer(std::uint32_t peerNodeId) const;

    /**
     * 淘汰超时的会话，只处理未建立的中间状态。
     *
     * @param now 当前时间。
     * @return 被淘汰的数量。
     */
    std::size_t removeExpired(std::chrono::steady_clock::time_point now);

    /**
     * 获取全部会话快照。
     *
     * @return 记录副本列表。
     */
    std::vector<SessionRecord> list() const;

    // 清空会话表。
    void clear();

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, SessionRecord> sessions_;
};

} // namespace netproxy::session
