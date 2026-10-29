#include <cstdint>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "net/Poller.h"
#include "net/TCPSocket.h"
#include "net/UDPSocket.h"

namespace {

using netproxy::net::IPAddress;
using netproxy::net::Poller;
using netproxy::net::TCPSocket;
using netproxy::net::UDPSocket;

void ipAddressFormatting() {
    const IPAddress address = IPAddress::parse("127.0.0.1", 8080);

    test::checkTrue(address.isValid(), "IPAddress 解析 IPv4 字面量");
    test::checkEquals(address.host(), std::string("127.0.0.1"), "IPAddress 输出主机");
    test::checkEquals(address.port(), 8080, "IPAddress 输出端口");
    test::checkEquals(address.toString(), std::string("127.0.0.1:8080"), "IPAddress 输出完整地址");

    const IPAddress any = IPAddress::any(0);
    test::checkEquals(any.host(), std::string("0.0.0.0"), "IPAddress::any 使用通配地址");

    test::checkFalse(IPAddress().isValid(), "默认 IPAddress 无效");
    test::checkEquals(IPAddress().toString(), std::string(), "无效 IPAddress 输出空串");
}

void tcpLoopbackTransfer() {
    TCPSocket listener;
    test::checkTrue(listener.bind(IPAddress::parse("127.0.0.1", 0)), "TCP bind 回环地址");
    test::checkTrue(listener.listen(4), "TCP listen");

    const std::uint16_t port = listener.localPort();
    test::checkTrue(port != 0, "TCP 获取自动分配的端口");

    TCPSocket client;
    test::checkTrue(client.connect(IPAddress::loopback(port)), "TCP connect 监听端口");

    TCPSocket connection = listener.accept();
    test::checkTrue(connection.isValid(), "TCP accept 返回有效连接");

    const std::string message = "ping";
    test::checkEquals(client.send(message), 4, "TCP send 返回发送字节数");

    char buffer[16] = {};
    const int received = connection.receive(buffer, sizeof(buffer));
    test::checkEquals(received, 4, "TCP receive 返回接收字节数");
    test::checkEquals(std::string(buffer, received > 0 ? static_cast<std::size_t>(received) : 0), message,
                      "TCP 收到完整消息");

    test::checkTrue(client.shutdownWrite(), "TCP shutdownWrite");
    test::checkEquals(connection.receive(buffer, sizeof(buffer)), 0, "对端关闭后 receive 返回 0");
}

void tcpOptions() {
    TCPSocket listener;
    test::checkTrue(listener.bind(IPAddress::any(0)), "TCP 绑定用于设置选项");
    test::checkTrue(listener.setNoDelay(true), "TCP 开启 NoDelay");
    test::checkTrue(listener.setReuseAddress(true), "TCP 开启地址复用");
}

void udpLoopbackTransfer() {
    UDPSocket receiver;
    test::checkTrue(receiver.bind(IPAddress::parse("127.0.0.1", 0)), "UDP bind 回环地址");

    const std::uint16_t port = receiver.localPort();
    test::checkTrue(port != 0, "UDP 获取自动分配的端口");

    UDPSocket sender;
    test::checkEquals(sender.sendTo(std::string("pong"), IPAddress::loopback(port)), 4, "UDP sendTo 返回发送字节数");

    char buffer[16] = {};
    IPAddress from;
    const int received = receiver.receiveFrom(buffer, sizeof(buffer), from);

    test::checkEquals(received, 4, "UDP receiveFrom 返回接收字节数");
    test::checkEquals(std::string(buffer, received > 0 ? static_cast<std::size_t>(received) : 0), std::string("pong"),
                      "UDP 收到完整消息");
    test::checkTrue(from.isValid(), "UDP 收到发送方地址");
    test::checkEquals(from.host(), std::string("127.0.0.1"), "UDP 发送方主机正确");
    test::checkTrue(from.port() != 0, "UDP 发送方端口非零");
}

void udpMulticastTransfer() {
    UDPSocket receiver;
    test::checkTrue(receiver.bind(IPAddress::any(0)), "UDP 组播: bind");

    const std::string group = "239.255.0.1";
    test::checkTrue(receiver.joinMulticastGroup(group), "UDP 组播: 加入组播组");
    test::checkTrue(receiver.setMulticastLoop(true), "UDP 组播: 开启组播回环");
    test::checkTrue(receiver.setMulticastTtl(1), "UDP 组播: 设置 TTL");

    UDPSocket sender;
    const int sent = sender.sendTo(std::string("mcast"), IPAddress::parse(group, receiver.localPort()));
    test::checkTrue(sent > 0, "UDP 组播: 发送数据");

    Poller poller;
    test::checkTrue(poller.add(receiver.nativeHandle(), Poller::Read), "UDP 组播: 注册读事件");

    std::vector<Poller::ReadyEvent> events;
    const int ready = poller.poll(1000, events);
    test::checkTrue(ready > 0, "UDP 组播: 等待可读");

    if (ready > 0) {
        char buffer[16] = {};
        IPAddress from;
        test::checkTrue(receiver.receiveFrom(buffer, sizeof(buffer), from) > 0, "UDP 组播: 收到数据");
    }

    test::checkTrue(receiver.leaveMulticastGroup(group), "UDP 组播: 离开组播组");
}

} // namespace

void runSocketTests() {
    NETPROXY_CASE(ipAddressFormatting);
    NETPROXY_CASE(tcpLoopbackTransfer);
    NETPROXY_CASE(tcpOptions);
    NETPROXY_CASE(udpLoopbackTransfer);
    NETPROXY_CASE(udpMulticastTransfer);
}
