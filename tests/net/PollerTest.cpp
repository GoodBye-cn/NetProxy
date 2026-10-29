#include <chrono>
#include <thread>
#include <vector>

#include "TestSupport.h"
#include "net/Poller.h"
#include "net/TCPSocket.h"

namespace {

using netproxy::net::IPAddress;
using netproxy::net::Poller;
using netproxy::net::TCPSocket;

void pollReadiness() {
    TCPSocket listener;
    test::checkTrue(listener.bind(IPAddress::loopback(0)), "Poller: bind 监听套接字");
    test::checkTrue(listener.listen(1), "Poller: listen");

    TCPSocket client;
    test::checkTrue(client.connect(IPAddress::loopback(listener.localPort())), "Poller: connect");

    TCPSocket server = listener.accept();
    test::checkTrue(server.isValid(), "Poller: accept");

    Poller poller;
    test::checkTrue(poller.add(server.nativeHandle(), Poller::Read), "Poller: 添加读事件");
    test::checkEquals(static_cast<int>(poller.size()), 1, "Poller: 事件数量");
    test::checkTrue(poller.contains(server.nativeHandle()), "Poller: 包含已注册句柄");

    std::vector<Poller::ReadyEvent> events;
    test::checkEquals(poller.poll(0, events), 0, "Poller: 无数据时立即超时");
    test::checkTrue(events.empty(), "Poller: 超时时无就绪事件");

    test::checkEquals(client.send("x"), 1, "Poller: 发送数据");
    test::checkEquals(poller.poll(1000, events), 1, "Poller: 有数据时返回就绪数量");
    test::checkEquals(static_cast<int>(events.size()), 1, "Poller: 就绪事件条目数");

    const bool hasReadEvent = !events.empty() && (events[0].events & Poller::Read) != 0;
    test::checkTrue(hasReadEvent, "Poller: 就绪事件包含可读");

    test::checkTrue(poller.modify(server.nativeHandle(), Poller::Write), "Poller: 修改为写事件");
    test::checkTrue(poller.remove(server.nativeHandle()), "Poller: 移除事件");
    test::checkTrue(poller.empty(), "Poller: 移除后为空");
    test::checkFalse(poller.contains(server.nativeHandle()), "Poller: 移除后不再包含");
}

void pollWakeup() {
    TCPSocket listener;
    test::checkTrue(listener.bind(IPAddress::loopback(0)), "Poller 多线程: bind");
    test::checkTrue(listener.listen(1), "Poller 多线程: listen");

    TCPSocket client;
    test::checkTrue(client.connect(IPAddress::loopback(listener.localPort())), "Poller 多线程: connect");

    TCPSocket server = listener.accept();
    test::checkTrue(server.isValid(), "Poller 多线程: accept");
    test::checkEquals(client.send("x"), 1, "Poller 多线程: 发送数据");

    Poller poller;
    std::thread registrar([&poller, &server] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        poller.add(server.nativeHandle(), Poller::Read);
    });

    std::vector<Poller::ReadyEvent> events;
    bool readable = false;
    for (int attempt = 0; attempt < 5 && !readable; ++attempt) {
        const int count = poller.poll(2000, events);
        test::checkTrue(count >= 0, "Poller 多线程: poll 不失败");

        for (const Poller::ReadyEvent& event : events) {
            if ((event.events & Poller::Read) != 0) {
                readable = true;
            }
        }
    }

    registrar.join();
    test::checkTrue(readable, "Poller 多线程: 注册后拿到可读事件");
}

void wakeupInterruptsBlockingPoll() {
    Poller poller;
    std::thread waker([&poller] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        poller.wakeup();
    });

    std::vector<Poller::ReadyEvent> events;
    const auto start = std::chrono::steady_clock::now();
    const int count = poller.poll(5000, events);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    waker.join();

    test::checkEquals(count, 0, "Poller: 仅唤醒时返回 0");
    test::checkTrue(elapsed.count() < 2000, "Poller: 唤醒打断阻塞 poll");
}

} // namespace

void runPollTests() {
    NETPROXY_CASE(pollReadiness);
    NETPROXY_CASE(pollWakeup);
    NETPROXY_CASE(wakeupInterruptsBlockingPoll);
}
