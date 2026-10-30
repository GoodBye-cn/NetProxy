#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "TransportTypes.h"
#include "core/IoLoop.h"
#include "net/Socket.h"
#include "net/UDPSocket.h"

namespace netproxy::transport {

/**
 * 数据 UDP socket 的封装。
 *
 * 负责绑定、读事件注册与发送投递，不解析报文内容。
 * socket 的读写都在 I/O 线程完成。
 */
class DataChannel final {
public:
    // 收到报文后的回调，运行在 I/O 线程。
    using Receiver = std::function<void(const net::IPAddress& from, const char* data, std::size_t size)>;

    /**
     * 构造数据通道，不创建套接字。
     *
     * @param io I/O 线程循环，必须比本对象活得久。
     */
    explicit DataChannel(core::IoLoop& io);

    // 析构时关闭通道。
    ~DataChannel();

    DataChannel(const DataChannel&) = delete;
    DataChannel& operator=(const DataChannel&) = delete;

    /**
     * 打开数据 socket。
     *
     * @param bindAddress 绑定地址，空串表示 0.0.0.0。
     * @param port 绑定端口，0 表示由系统分配。
     * @return 打开成功返回 true；地址无效或绑定失败返回 false。
     * @note 需在 IoLoop 启动前调用。
     */
    bool open(const std::string& bindAddress, std::uint16_t port);

    // 注销读事件并关闭套接字。
    void close();

    /**
     * 判断通道是否已打开。
     *
     * @return 已成功调用 open 返回 true。
     */
    bool isOpen() const;

    /**
     * 获取实际绑定的端口。
     *
     * @return 系统分配或配置的端口；未打开返回 0。
     */
    std::uint16_t localPort() const;

    /**
     * 设置报文接收回调并注册读事件。
     *
     * @param receiver 回调对象，为空表示不再处理收到的报文。
     * @return 注册成功返回 true；未打开或注册失败返回 false。
     * @note 需在 IoLoop 启动前调用。
     */
    bool setReceiver(Receiver receiver);

    /**
     * 发送报文，线程安全。
     *
     * @param packet 已编码的报文内容。
     * @param target 目标地址。
     */
    void send(const std::string& packet, const net::IPAddress& target);

    /**
     * 获取绑定到本通道的发送函数。
     *
     * @return 可直接注入给 `PeerConnection` 的回调，捕获本对象指针。
     * @note 调用方需保证本对象比使用者活得久。
     */
    PacketSender sender();

private:
    // I/O 线程：读取全部已到达的报文并回调。
    void handleReadable();

    core::IoLoop& io_;
    net::UDPSocket socket_;
    Receiver receiver_;
    bool opened_ = false;
    bool registered_ = false;
};

} // namespace netproxy::transport
