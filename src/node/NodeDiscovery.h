#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "DiscoveryProtocol.h"
#include "PeerInfo.h"
#include "core/IoLoop.h"
#include "net/UDPSocket.h"

namespace netproxy::node {

// 内部沿用 node 命名空间下的名字，公共定义在 PeerInfo.h
using ::netproxy::PeerInfo;

/**
 * 节点发现通道：持有 UDP 组播 socket，负责公告的收发与解析。
 *
 * 只处理本网段的 Announce / Bye，不用于点对点通信。
 * socket 的所有读写都在 I/O 线程完成。
 */
class NodeDiscovery final {
public:
    // 收到公告后的回调，运行在 I/O 线程。
    using Receiver = std::function<void(const PeerInfo& peer)>;

    static constexpr const char* DEFAULT_MULTICAST_GROUP = "239.255.42.99";
    static constexpr std::uint16_t DEFAULT_MULTICAST_PORT = 45454;

    /**
     * 构造发现通道，不创建套接字。
     *
     * @param io I/O 线程循环，必须比本对象活得久。
     * @param nodeId 本节点标识，用于过滤自身公告。
     * @param group 组播组，空串使用默认值。
     * @param port 组播端口，0 使用默认值。
     */
    NodeDiscovery(core::IoLoop& io, std::uint32_t nodeId, const std::string& group, std::uint16_t port);

    // 析构时关闭通道。
    ~NodeDiscovery();

    NodeDiscovery(const NodeDiscovery&) = delete;
    NodeDiscovery& operator=(const NodeDiscovery&) = delete;

    /**
     * 打开组播套接字。
     *
     * 绑定组播端口、加入组播组、设置 TTL 1 与非阻塞。
     *
     * @param announcePort 对外公告的数据端口。
     * @return 打开成功返回 true；绑定或加入组播失败返回 false，并已记录日志。
     * @note 需在 IoLoop 启动前调用。
     */
    bool open(std::uint16_t announcePort);

    // 注销读事件并关闭套接字。
    void close();

    /**
     * 判断通道是否已打开。
     *
     * @return 已成功调用 open 返回 true。
     */
    bool isOpen() const;

    /**
     * 组播发送一次公告。
     *
     * @return 发送成功返回 true；未打开或发送失败返回 false。
     * @note 应在 I/O 线程调用，通常通过 IoLoop::post 触发。
     */
    bool announce();

    /**
     * 组播发送一次离开通知。
     *
     * @return 发送成功返回 true；未打开或发送失败返回 false。
     */
    bool sendBye();

    /**
     * 设置公告接收回调并注册读事件。
     *
     * @param receiver 回调对象，为空表示不再处理收到的公告。
     * @return 注册成功返回 true；未打开或注册失败返回 false。
     * @note 需在 IoLoop 启动前调用。
     */
    bool setReceiver(Receiver receiver);

    /**
     * 获取公告的数据端口。
     *
     * @return open 时传入的端口。
     */
    std::uint16_t announcePort() const;

private:
    // I/O 线程：读事件入口，读取全部已到达的公告并回调；事件参数不使用。
    void onEvent(net::SocketHandle, std::uint32_t);

    // 构造并组播发送指定类型的报文。
    bool sendMessage(AnnounceType type);

    core::IoLoop& io_;                // I/O 线程循环，收发都在此线程
    net::UDPSocket socket_;           // 组播 socket
    net::IPAddress multicastAddress_; // 解析后的组播地址
    std::string multicastGroup_;      // 组播组字面量
    std::uint16_t multicastPort_;     // 组播端口
    std::uint32_t nodeId_ = 0;        // 本节点标识，写入公告并用于过滤自身
    std::uint16_t announcePort_ = 0;  // 公告中的数据端口
    Receiver receiver_;               // 收到公告后的回调
    bool opened_ = false;             // socket 是否已打开
    bool registered_ = false;         // 读事件是否已注册
};

} // namespace netproxy::node
