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

// 回归：对端发起的流必须出现在本地流表中，并且可以回发数据。
void servicePeerInitiatedStream() {
    netproxy::P2PServiceConfig configA;
    configA.nodeId = 0x50505050;
    configA.dataPort = 9531;
    configA.announceIntervalMs = 100;

    netproxy::P2PServiceConfig configB = configA;
    configB.nodeId = 0x60606060;
    configB.dataPort = 9532;

    netproxy::P2PService serviceA;
    netproxy::P2PService serviceB;

    std::atomic<std::uint32_t> openedOnB{0};
    serviceB.setStreamHandler([&openedOnB](std::uint32_t streamId, std::uint32_t, netproxy::StreamEvent event,
                                           std::uint8_t) {
        if (event == netproxy::StreamEvent::Opened) {
            openedOnB.store(streamId);
        }
    });

    std::mutex dataMutex;
    std::string payload;
    std::atomic<int> received{0};
    serviceA.setDataHandler(
        [&dataMutex, &payload, &received](std::uint32_t, std::uint32_t, const char* data, std::size_t size) {
            {
                std::lock_guard<std::mutex> lock(dataMutex);
                payload.assign(data, size);
            }
            received.fetch_add(1);
        });

    test::checkTrue(serviceA.start(configA), "对端流: A 启动");
    test::checkTrue(serviceB.start(configB), "对端流: B 启动");

    netproxy::PeerInfo peer;
    bool discovered = false;
    for (int attempt = 0; attempt < 40 && !discovered; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        discovered = serviceA.findPeer(configB.nodeId, peer);
    }
    test::checkTrue(discovered, "对端流: A 发现 B");

    const std::uint32_t streamId = serviceA.openStream(configB.nodeId);
    test::checkTrue(streamId != 0, "对端流: 发起成功");

    bool accepted = false;
    for (int attempt = 0; attempt < 40 && !accepted; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        accepted = openedOnB.load() == streamId;
    }
    test::checkTrue(accepted, "对端流: B 收到 Opened");

    bool visible = false;
    for (int attempt = 0; attempt < 20 && !visible; ++attempt) {
        visible = serviceB.isStreamOpen(streamId);
        if (!visible) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    test::checkTrue(visible, "对端流: B 侧 isStreamOpen 为真");

    bool listed = false;
    for (int attempt = 0; attempt < 20 && !listed; ++attempt) {
        for (const netproxy::StreamInfo& info : serviceB.streams()) {
            if (info.streamId == streamId && info.open && info.peerNodeId == configA.nodeId) {
                listed = true;
            }
        }
        if (!listed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    test::checkTrue(listed, "对端流: B 侧列表包含该流");

    test::checkTrue(serviceB.sendData(streamId, "pong", 4), "对端流: B 回发受理");

    bool delivered = false;
    for (int attempt = 0; attempt < 40 && !delivered; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        delivered = received.load() > 0;
    }
    test::checkTrue(delivered, "对端流: A 收到回发数据");

    {
        std::lock_guard<std::mutex> lock(dataMutex);
        test::checkEquals(payload, std::string("pong"), "对端流: 回发内容一致");
    }

    serviceA.stop();
    serviceB.stop();
}

// 回归：数据回调里直接回发不应丢失或阻塞。
void serviceDataHandlerReentrant() {
    netproxy::P2PServiceConfig configA;
    configA.nodeId = 0x70707070;
    configA.dataPort = 9541;
    configA.announceIntervalMs = 100;

    netproxy::P2PServiceConfig configB = configA;
    configB.nodeId = 0x80808080;
    configB.dataPort = 9542;

    netproxy::P2PService serviceA;
    netproxy::P2PService serviceB;

    std::atomic<int> pong{0};
    serviceB.setDataHandler(
        [&pong](std::uint32_t, std::uint32_t, const char*, std::size_t) { pong.fetch_add(1); });

    serviceA.setDataHandler([&serviceA](std::uint32_t streamId, std::uint32_t, const char*, std::size_t) {
        test::checkTrue(serviceA.sendData(streamId, "pong", 4), "重入: 回调内回发受理");
    });

    test::checkTrue(serviceA.start(configA), "重入: A 启动");
    test::checkTrue(serviceB.start(configB), "重入: B 启动");

    netproxy::PeerInfo peer;
    bool discovered = false;
    for (int attempt = 0; attempt < 40 && !discovered; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        discovered = serviceA.findPeer(configB.nodeId, peer);
    }
    test::checkTrue(discovered, "重入: A 发现 B");

    const std::uint32_t streamId = serviceA.openStream(configB.nodeId);
    test::checkTrue(streamId != 0, "重入: 发起成功");

    bool established = false;
    for (int attempt = 0; attempt < 40 && !established; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        established = serviceA.isStreamOpen(streamId) && serviceB.isStreamOpen(streamId);
    }
    test::checkTrue(established, "重入: 双方流已打开");

    test::checkTrue(serviceB.sendData(streamId, "ping", 4), "重入: B 发送成功");

    bool replayed = false;
    for (int attempt = 0; attempt < 40 && !replayed; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        replayed = pong.load() > 0;
    }
    test::checkTrue(replayed, "重入: 回调内回发已送达");

    serviceA.stop();
    serviceB.stop();
}

} // namespace

void runP2PServiceTests() {
    NETPROXY_CASE(serviceLifecycle);
    NETPROXY_CASE(serviceStream);
    NETPROXY_CASE(serviceDataTransfer);
    NETPROXY_CASE(servicePeerInitiatedStream);
    NETPROXY_CASE(serviceDataHandlerReentrant);
}
