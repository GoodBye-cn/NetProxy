#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

#include "Poller.h"
#include "Socket.h"

namespace netproxy::net {

/**
 * 事件分发器，把 Poller 返回的就绪事件派发给注册的回调。
 *
 * 内部持有 Poller，另存一份「句柄 -> 回调」的映射。
 * 回调在锁外执行，因此允许在回调内部再次 add / modify / remove。
 * 注册表可由任意线程修改，回调只在调用 dispatch() 的线程上执行。
 */
class EventDispatcher final {
public:
    /**
     * 事件回调。
     *
     * @param handle 就绪的句柄。
     * @param events 就绪事件位标志，Poller::Read / Write / Error 的组合。
     */
    using Handler = std::function<void(SocketHandle handle, std::uint32_t events)>;

    // 构造分发器，内部创建唤醒通道。
    EventDispatcher() = default;

    // 析构时关闭 Poller 与唤醒通道。
    ~EventDispatcher() = default;

    EventDispatcher(const EventDispatcher&) = delete;
    EventDispatcher& operator=(const EventDispatcher&) = delete;

    /**
     * 注册句柄、关注事件与回调。
     *
     * @param handle 待监听的句柄。
     * @param events 关注的事件位标志，如 Poller::Read | Poller::Write。
     * @param handler 事件就绪时调用的回调，不能为空。
     * @return 注册成功返回 true；句柄无效、回调为空或句柄已注册返回 false，并已记录日志。
     */
    bool add(SocketHandle handle, std::uint32_t events, Handler handler);

    /**
     * 修改已注册句柄的关注事件，回调保持不变。
     *
     * @param handle 已注册的句柄。
     * @param events 新的事件位标志。
     * @return 修改成功返回 true；句柄未注册返回 false。
     */
    bool modify(SocketHandle handle, std::uint32_t events);

    /**
     * 移除句柄及其回调。
     *
     * @param handle 待移除的句柄。
     * @return 从 Poller 中移除成功返回 true；句柄未注册返回 false。
     * @note 移除后即使还有已就绪但尚未分发的事件，也会被跳过。
     */
    bool remove(SocketHandle handle);

    /**
     * 判断句柄是否已注册。
     *
     * @param handle 待查询的句柄。
     * @return 已注册返回 true。
     */
    bool contains(SocketHandle handle) const;

    /**
     * 获取已注册句柄数量。
     *
     * @return 当前回调数量。
     */
    std::size_t size() const;

    // 清空注册表，并唤醒阻塞中的 dispatch。
    void clear();

    /**
     * 等待事件并分发。
     *
     * 调用 Poller::poll 取得就绪事件，逐个查找回调后在锁外调用。
     *
     * @param timeoutMs 超时毫秒数，负数表示无限等待。
     * @return 触发的回调数量；超时或被唤醒返回 0；poll 失败返回 -1，并已记录日志。
     * @note 被唤醒但没有就绪事件时返回 0，调用方应在循环中继续调用。
     */
    int dispatch(int timeoutMs);

    // 打断阻塞中的 dispatch，使其立即返回 0。
    void wakeup();

private:
    mutable std::mutex mutex_;
    std::unordered_map<SocketHandle, Handler> handlers_;
    Poller poller_;
};

} // namespace netproxy::net
