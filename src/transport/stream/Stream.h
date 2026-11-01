#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "Protocol.h"
#include "StreamTypes.h"

namespace netproxy::transport {

/**
 * 单条流。
 *
 * 只维护状态与回调，不持有连接、不做收发调度：所有帧统一经 `StreamMux` 注入的
 * `FrameSender` 发送，保证进入同一条可靠有序队列。
 *
 * 线程模型：不做内部加锁，由 `StreamMux` 的互斥量覆盖；回调应在锁外触发。
 */
class Stream final {
public:
    // 流事件：Opened / Rejected / Closed。
    using EventCallback =
        std::function<void(std::uint32_t streamId, protocol::StreamEvent event, protocol::CloseReason reason)>;

    // 数据交付：回调返回后 data 失效。
    using DataCallback = std::function<void(std::uint32_t streamId, const char* data, std::size_t size)>;

    /**
     * @param streamId 流标识，非 0。
     * @param peerNodeId 所属对端。
     * @param locallyInitiated true 表示本端发起（等待 Accept），false 表示对端发起。
     */
    Stream(std::uint32_t streamId, std::uint32_t peerNodeId, bool locallyInitiated);

    std::uint32_t id() const;
    std::uint32_t peerNodeId() const;
    StreamState state() const;
    bool isOpen() const;
    bool locallyInitiated() const;

    void setEventCallback(EventCallback handler);
    void setDataCallback(DataCallback handler);

    // Opening -> Open；返回状态是否发生变化。
    bool markOpen();

    // 把数据交给 DataCallback；流已关闭时丢弃。
    void deliverData(const char* data, std::size_t size);

    // 置 Closed 并回调一次事件。
    void close(protocol::StreamEvent event, protocol::CloseReason reason);

private:
    std::uint32_t streamId_ = 0;
    std::uint32_t peerNodeId_ = 0;
    bool locallyInitiated_ = false;
    StreamState state_ = StreamState::Opening;
    EventCallback eventHandler_;
    DataCallback dataHandler_;
};

} // namespace netproxy::transport
