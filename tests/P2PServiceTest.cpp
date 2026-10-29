#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "P2PService.h"
#include "TestSupport.h"

namespace {

void serviceLifecycle() {
    netproxy::P2PService service;

    test::checkFalse(service.isRunning(), "服务: 初始未运行");

    netproxy::P2PServiceConfig config;
    config.nodeId = 0;
    config.dataPort = 0;

    test::checkTrue(service.start(config), "服务: 启动成功");
    test::checkTrue(service.isRunning(), "服务: 运行状态正确");
    test::checkTrue(service.nodeId() != 0, "服务: 随机分配 nodeId");
    test::checkTrue(service.dataPort() != 0, "服务: 系统分配数据端口");

    service.stop();
    test::checkFalse(service.isRunning(), "服务: 停止后状态正确");
}

void serviceDataTransfer() {
    netproxy::P2PServiceConfig configA;
    configA.nodeId = 0x30303030;
    configA.dataPort = 9521;
    configA.announceIntervalMs = 100;

    netproxy::P2PServiceConfig configB = configA;
    configB.nodeId = 0x40404040;
    configB.dataPort = 9522;

    netproxy::P2PService serviceA;
    netproxy::P2PService serviceB;

    std::mutex dataMutex;
    std::string payload;
    std::atomic<int> received{0};

    serviceB.setDataHandler(
        [&dataMutex, &payload, &received](std::uint32_t, std::uint32_t, const char* data, std::size_t size) {
            {
                std::lock_guard<std::mutex> lock(dataMutex);
                payload.assign(data, size);
            }
            received.fetch_add(1);
        });

    test::checkTrue(serviceA.start(configA), "数据: A 启动");
    test::checkTrue(serviceB.start(configB), "数据: B 启动");

    netproxy::PeerInfo peer;
    bool discovered = false;
    for (int attempt = 0; attempt < 40 && !discovered; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        discovered = serviceA.findPeer(configB.nodeId, peer);
    }
    test::checkTrue(discovered, "数据: A 发现 B");

    const std::uint32_t streamId = serviceA.openStream(configB.nodeId);
    test::checkTrue(streamId != 0, "数据: 开流成功");

    bool opened = false;
    for (int attempt = 0; attempt < 40 && !opened; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        opened = serviceA.isStreamOpen(streamId);
    }
    test::checkTrue(opened, "数据: 流已打开");

    test::checkTrue(serviceA.sendData(streamId, "hello", 5), "数据: 提交成功");

    bool delivered = false;
    for (int attempt = 0; attempt < 40 && !delivered; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        delivered = received.load() > 0;
    }
    test::checkTrue(delivered, "数据: 对端收到");

    {
        std::lock_guard<std::mutex> lock(dataMutex);
        test::checkEquals(payload, std::string("hello"), "数据: 内容一致");
    }

    test::checkFalse(serviceA.sendData(streamId, nullptr, 0), "数据: 空数据被拒绝");

    serviceA.stop();
    serviceB.stop();
}

void serviceStream() {
    netproxy::P2PServiceConfig configA;
    configA.nodeId = 0x10101010;
    configA.dataPort = 9501;
    configA.announceIntervalMs = 100;

    netproxy::P2PServiceConfig configB = configA;
    configB.nodeId = 0x20202020;
    configB.dataPort = 9502;

    netproxy::P2PService serviceA;
    netproxy::P2PService serviceB;

    std::atomic<int> openedB{0};
    serviceB.setStreamHandler([&openedB](std::uint32_t, std::uint32_t, netproxy::StreamEvent event, std::uint8_t) {
        if (event == netproxy::StreamEvent::Opened) {
            openedB.fetch_add(1);
        }
    });

    test::checkTrue(serviceA.start(configA), "服务流: A 启动");
    test::checkTrue(serviceB.start(configB), "服务流: B 启动");

    netproxy::PeerInfo peer;
    bool discovered = false;
    for (int attempt = 0; attempt < 40 && !discovered; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        discovered = serviceA.findPeer(configB.nodeId, peer);
    }
    test::checkTrue(discovered, "服务流: A 发现 B");

    const std::uint32_t streamId = serviceA.openStream(configB.nodeId);
    test::checkTrue(streamId != 0, "服务流: 发起成功");

    bool established = false;
    for (int attempt = 0; attempt < 40 && !established; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        established = serviceA.isStreamOpen(streamId) && openedB.load() > 0;
    }
    test::checkTrue(established, "服务流: 双方打开完成");

    const std::vector<netproxy::StreamInfo> active = serviceA.streams();
    test::checkEquals(static_cast<int>(active.size()), 1, "服务流: 列表包含一条流");
    if (!active.empty()) {
        test::checkTrue(active[0].open, "服务流: 列表状态为已打开");
        test::checkEquals(static_cast<int>(active[0].peerNodeId), static_cast<int>(configB.nodeId),
                          "服务流: 列表对端标识正确");
    }

    test::checkTrue(serviceA.closeStream(streamId), "服务流: 关闭请求已投递");

    bool cleared = false;
    for (int attempt = 0; attempt < 20 && !cleared; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        cleared = serviceA.streams().empty();
    }
    test::checkTrue(cleared, "服务流: 关闭后列表为空");

    serviceA.stop();
    serviceB.stop();
}

} // namespace

void runP2PServiceTests() {
    NETPROXY_CASE(serviceLifecycle);
    NETPROXY_CASE(serviceStream);
    NETPROXY_CASE(serviceDataTransfer);
}
