#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

#include "DataStream.h"
#include "net/Socket.h"

namespace netproxy::transport {

/**
 * 按会话标识管理 DataStream。
 *
 * 会话建立时创建，关闭或断链时销毁；报文解析、交付与断链回调都在这里汇聚。
 */
class DataStreamManager final {
public:
    using Sender = DataStream::Sender;
    using Delivered = std::function<void(std::uint32_t sessionId, const char* data, std::size_t size)>;
    using Closed = std::function<void(std::uint32_t sessionId, protocol::CloseReason reason)>;

    /**
     * 构造管理器。
     *
     * @param sender 报文发送函数，通常由会话层注入 socket 发送。
     * @param localNodeId 本节点标识，写入报文头。
     * @param config 传输配置。
     */
    DataStreamManager(Sender sender, std::uint32_t localNodeId, const TransportConfig& config);

    // 析构时销毁全部数据流。
    ~DataStreamManager();

    DataStreamManager(const DataStreamManager&) = delete;
    DataStreamManager& operator=(const DataStreamManager&) = delete;

    /**
     * 为指定会话创建数据流，重复创建会被忽略。
     *
     * @param sessionId 会话标识。
     * @param peerNodeId 对端节点标识。
     * @param peer 对端地址与端口。
     */
    void create(std::uint32_t sessionId, std::uint32_t peerNodeId, const net::IPAddress& peer);

    /**
     * 销毁指定会话的数据流。
     *
     * @param sessionId 会话标识。
     */
    void remove(std::uint32_t sessionId);

    /**
     * 提交数据到指定会话。
     *
     * @param sessionId 会话标识。
     * @param data 数据内容。
     * @param size 长度。
     * @return 会话存在且数据已受理返回 true。
     */
    bool send(std::uint32_t sessionId, const void* data, std::size_t size);

    /**
     * 处理收到的数据报文，由会话层在业务线程调用。
     *
     * @param from 报文来源地址。
     * @param data 原始报文。
     * @param size 报文长度。
     */
    void onDatagram(const net::IPAddress& from, const char* data, std::size_t size);

    /**
     * 周期重传检查，业务线程调用。
     */
    void tick();

    // 销毁全部数据流。
    void clear();

    void setDeliveredHandler(Delivered handler);
    void setClosedHandler(Closed handler);

    /**
     * 获取当前数据流数量。
     *
     * @return 已创建的会话数。
     */
    std::size_t size() const;

private:
    std::uint32_t localNodeId_ = 0;
    TransportConfig config_;
    Sender sender_;
    std::unordered_map<std::uint32_t, std::shared_ptr<DataStream>> streams_;
    Delivered delivered_;
    Closed closed_;
};

} // namespace netproxy::transport
