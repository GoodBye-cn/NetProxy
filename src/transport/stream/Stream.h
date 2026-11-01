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
     * 构造一条流，初始状态为 Opening。
     *
     * @param streamId 流标识，非 0，由发起方分配。
     * @param peerNodeId 所属对端节点标识。
     * @param locallyInitiated true 表示本端发起（等待 Accept），false 表示对端发起。
     */
    Stream(std::uint32_t streamId, std::uint32_t peerNodeId, bool locallyInitiated);

    /**
     * 获取流标识。
     *
     * @return 构造时传入的流号。
     */
    std::uint32_t id() const;

    /**
     * 获取所属对端。
     *
     * @return 对端节点标识。
     */
    std::uint32_t peerNodeId() const;

    /**
     * 获取当前状态。
     *
     * @return Opening / Open / Closing / Closed。
     * @note 状态只在 `StreamMux` 的锁内推进。
     */
    StreamState state() const;

    /**
     * 判断流是否可双向传数据。
     *
     * @return 状态为 Open 时返回 true。
     */
    bool isOpen() const;

    /**
     * 判断发起方向。
     *
     * @return true 表示本端发起（经 Opening 等 Accept），false 表示对端发起。
     */
    bool locallyInitiated() const;

    /**
     * 设置流事件回调。
     *
     * @param handler 回调对象；为空表示不通知。
     */
    void setEventCallback(EventCallback handler);

    /**
     * 设置数据交付回调。
     *
     * @param handler 回调对象；为空表示丢弃收到的数据。
     */
    void setDataCallback(DataCallback handler);

    /**
     * 把流从 Opening 推进到 Open。
     *
     * @return 状态发生变化返回 true；已不是 Opening 时返回 false 且不改状态。
     */
    bool markOpen();

    /**
     * 把一段数据交给数据回调。
     *
     * @param data 负载首地址，回调返回后失效。
     * @param size 负载长度，由调用方保证非 0。
     * @note 流已 Closed 或未设置回调时直接丢弃。
     */
    void deliverData(const char* data, std::size_t size);

    /**
     * 置为 Closed 并回调一次事件。
     *
     * @param event 回调事件（Closed / Rejected）。
     * @param reason 关闭原因。
     * @note 重复调用不会重复回调。
     */
    void close(protocol::StreamEvent event, protocol::CloseReason reason);

private:
    std::uint32_t streamId_ = 0;               // 流标识，非 0
    std::uint32_t peerNodeId_ = 0;             // 所属对端
    bool locallyInitiated_ = false;            // 是否本端发起
    StreamState state_ = StreamState::Opening; // 初始为 Opening，对端发起的流由 Mux 直接置 Open
    EventCallback eventHandler_;               // 流事件回调
    DataCallback dataHandler_;                 // 数据回调
};

} // namespace netproxy::transport
