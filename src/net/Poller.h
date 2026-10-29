#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "Socket.h"
#include "WakeupChannel.h"

#ifndef _WIN32
#include <poll.h>
#endif

namespace netproxy::net {

#ifdef _WIN32
using PollDescriptor = WSAPOLLFD;
#else
using PollDescriptor = pollfd;
#endif

/**
 * poll / WSAPoll 的封装，支持多线程使用。
 *
 * 注册表由互斥量保护，poll 在锁外阻塞：每次调用先复制当前注册表快照，
 * 再把唤醒通道放到首位。其它线程增删句柄或调用 wakeup 时写入唤醒通道，
 * 正在阻塞的 poll 会立即返回。
 */
class Poller final {
public:
    // 关注的事件，可按位组合。
    enum Event : std::uint32_t {
        Read = 1U << 0,  // 可读
        Write = 1U << 1, // 可写
        Error = 1U << 2  // 出错或连接关闭
    };

    // 一条就绪结果。
    struct ReadyEvent {
        SocketHandle handle = INVALID_SOCKET_HANDLE; // 就绪的句柄
        std::uint32_t events = 0;                    // 就绪的事件位标志
    };

    /**
     * 创建唤醒通道。
     *
     * 通道创建失败时仍可构造，但后续 poll 会返回 -1 并记录错误日志。
     */
    Poller();

    // 关闭唤醒通道。
    ~Poller();

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    /**
     * 注册句柄与关注事件。
     *
     * @param handle 待监听的句柄。
     * @param events 关注的事件位标志，如 Read | Write。
     * @return 注册成功返回 true；句柄无效或已注册返回 false，并已记录日志。
     */
    bool add(SocketHandle handle, std::uint32_t events);

    /**
     * 修改已注册句柄的关注事件。
     *
     * @param handle 已注册的句柄。
     * @param events 新的事件位标志。
     * @return 修改成功返回 true；句柄未注册返回 false，并已记录日志。
     */
    bool modify(SocketHandle handle, std::uint32_t events);

    /**
     * 移除已注册句柄。
     *
     * @param handle 待移除的句柄。
     * @return 移除成功返回 true；句柄未注册返回 false，并已记录日志。
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
     * @return 注册表当前大小。
     */
    std::size_t size() const;

    /**
     * 判断注册表是否为空。
     *
     * @return 没有注册任何句柄返回 true。
     */
    bool empty() const;

    // 清空注册表并唤醒阻塞中的 poll。
    void clear();

    // 打断阻塞中的 poll，使其立即返回。
    void wakeup();

    /**
     * 等待事件就绪。
     *
     * 每次调用都复制当前注册表的快照，因此阻塞期间其它线程的增删只对下一次调用生效。
     *
     * @param timeoutMs 超时毫秒数，负数表示无限等待。
     * @param readyEvents 输出就绪事件列表，调用前会被清空。
     * @return 就绪事件数；超时或被唤醒返回 0；失败返回 -1，并已记录日志。
     * @note 被唤醒但没有用户事件时同样返回 0，调用方应在循环中继续调用。
     */
    int poll(int timeoutMs, std::vector<ReadyEvent>& readyEvents);

private:
    /**
     * 将位标志转换为平台事件掩码。
     *
     * @param events Read / Write 的组合。
     * @return POLLIN / POLLOUT 的组合。
     */
    static short toNativeEvents(std::uint32_t events);

    /**
     * 将平台事件掩码转换为位标志。
     *
     * @param nativeEvents poll 返回的 revents。
     * @return ReadyEvent 使用的事件位标志。
     */
    static std::uint32_t toEvents(short nativeEvents);

    /**
     * 查找句柄在注册表中的下标。
     *
     * @param handle 待查找的句柄。
     * @return 下标；未注册返回 -1。调用方必须已持有互斥量。
     */
    std::ptrdiff_t indexOf(SocketHandle handle) const;

    mutable std::mutex mutex_;
    std::vector<PollDescriptor> entries_;
    WakeupChannel wakeup_;
};

} // namespace netproxy::net
