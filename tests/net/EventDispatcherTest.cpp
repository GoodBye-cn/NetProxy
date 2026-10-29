#include <chrono>
#include <cstdint>
#include <thread>

#include "TestSupport.h"
#include "net/EventDispatcher.h"
#include "net/TCPSocket.h"

namespace {

using netproxy::net::EventDispatcher;
using netproxy::net::IPAddress;
using netproxy::net::Poller;
using netproxy::net::SocketHandle;
using netproxy::net::TCPSocket;

// 建立一对已连接的 TCP 套接字，client 用于发送，server 用于接收。
bool connectPair(TCPSocket& client, TCPSocket& server) {
    TCPSocket listener;
    if (!listener.bind(IPAddress::loopback(0)) || !listener.listen(1)) {
        return false;
    }

    if (!client.connect(IPAddress::loopback(listener.localPort()))) {
        return false;
    }

    server = listener.accept();
    return server.isValid();
}

void dispatchReadEvent() {
    TCPSocket client;
    TCPSocket server;
    test::checkTrue(connectPair(client, server), "分发器: 建立连接");

    EventDispatcher dispatcher;

    SocketHandle receivedHandle = netproxy::net::INVALID_SOCKET_HANDLE;
    std::uint32_t receivedEvents = 0;

    test::checkTrue(dispatcher.add(server.nativeHandle(), Poller::Read,
                                   [&](SocketHandle handle, std::uint32_t events) {
                                       receivedHandle = handle;
                                       receivedEvents = events;
                                   }),
                    "分发器: 注册读事件");
    test::checkEquals(static_cast<int>(dispatcher.size()), 1, "分发器: 注册数量");

    test::checkEquals(dispatcher.dispatch(0), 0, "分发器: 无事件时立即返回");

    test::checkEquals(client.send("x"), 1, "分发器: 发送数据");
    test::checkEquals(dispatcher.dispatch(1000), 1, "分发器: 触发一个回调");
    test::checkTrue((receivedEvents & Poller::Read) != 0, "分发器: 事件包含可读");
    test::checkTrue(receivedHandle == server.nativeHandle(), "分发器: 回调收到正确句柄");
}

void removeInsideHandler() {
    TCPSocket client;
    TCPSocket server;
    test::checkTrue(connectPair(client, server), "分发器回调: 建立连接");

    EventDispatcher dispatcher;
    int callCount = 0;

    test::checkTrue(dispatcher.add(server.nativeHandle(), Poller::Read,
                                   [&](SocketHandle handle, std::uint32_t) {
                                       ++callCount;
                                       dispatcher.remove(handle);
                                   }),
                    "分发器回调: 注册读事件");

    test::checkEquals(client.send("x"), 1, "分发器回调: 发送数据");
    test::checkEquals(dispatcher.dispatch(1000), 1, "分发器回调: 触发回调");
    test::checkFalse(dispatcher.contains(server.nativeHandle()), "分发器回调: 回调内移除自身");

    test::checkEquals(client.send("y"), 1, "分发器回调: 再次发送");
    test::checkEquals(dispatcher.dispatch(100), 0, "分发器回调: 移除后不再触发");
    test::checkEquals(callCount, 1, "分发器回调: 只执行一次");
}

void detectPeerClose() {
    TCPSocket client;
    TCPSocket server;
    test::checkTrue(connectPair(client, server), "分发器关闭: 建立连接");

    EventDispatcher dispatcher;
    bool closed = false;

    test::checkTrue(dispatcher.add(server.nativeHandle(), Poller::Read,
                                   [&](SocketHandle, std::uint32_t) {
                                       char buffer[8] = {};
                                       if (server.receive(buffer, sizeof(buffer)) == 0) {
                                           closed = true;
                                       }
                                   }),
                    "分发器关闭: 注册读事件");

    client.close();
    dispatcher.dispatch(1000);
    test::checkTrue(closed, "分发器关闭: 对端关闭后可读到 0");
}

void dispatcherWakeup() {
    EventDispatcher dispatcher;

    std::thread waker([&dispatcher] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        dispatcher.wakeup();
    });

    const auto start = std::chrono::steady_clock::now();
    const int count = dispatcher.dispatch(5000);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    waker.join();

    test::checkEquals(count, 0, "分发器: 唤醒时返回 0");
    test::checkTrue(elapsed.count() < 2000, "分发器: 唤醒打断阻塞");
}

} // namespace

void runDispatcherTests() {
    NETPROXY_CASE(dispatchReadEvent);
    NETPROXY_CASE(removeInsideHandler);
    NETPROXY_CASE(detectPeerClose);
    NETPROXY_CASE(dispatcherWakeup);
}
