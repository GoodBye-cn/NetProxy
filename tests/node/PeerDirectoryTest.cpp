#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "TestSupport.h"
#include "core/IoLoop.h"
#include "core/TaskLoop.h"
#include "node/NodeDiscovery.h"
#include "node/PeerDirectory.h"

namespace {

using netproxy::core::IoLoop;
using netproxy::core::TaskLoop;
using netproxy::node::DiscoveryConfig;
using netproxy::node::NodeDiscovery;
using netproxy::node::PeerDirectory;
using netproxy::node::PeerInfo;

// 单个节点的发现通道与管理器。
struct NodeRuntime final {
    std::uint16_t port = 0;
    IoLoop io;
    TaskLoop business;
    NodeDiscovery discovery;
    PeerDirectory manager;

    NodeRuntime(std::uint32_t nodeId, std::uint16_t listenPort, const DiscoveryConfig& config)
        : port(listenPort), discovery(io, nodeId, "", 0), manager(discovery, nodeId, config, io, business) {}

    NodeRuntime(const NodeRuntime&) = delete;
    NodeRuntime& operator=(const NodeRuntime&) = delete;

    bool start() {
        return discovery.open(port) && manager.start() && io.start() && business.start();
    }

    void stop() {
        business.stop();
        io.stop();
        manager.stop();
        discovery.close();
    }
};

void twoNodesDiscover() {
    DiscoveryConfig config;
    config.announceIntervalMs = 100;
    config.peerTimeoutMs = 2000;

    NodeRuntime nodeA(0xAAAA1111, 9001, config);
    NodeRuntime nodeB(0xBBBB2222, 9002, config);

    test::checkTrue(nodeA.start(), "节点: A 启动");
    test::checkTrue(nodeB.start(), "节点: B 启动");
    test::checkEquals(static_cast<int>(nodeA.manager.nodeId()), 0xAAAA1111, "节点: nodeId 正确");

    bool found = false;
    PeerInfo foundPeer;
    for (int attempt = 0; attempt < 20 && !found; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        found = nodeB.manager.findPeer(0xAAAA1111, foundPeer);
    }

    test::checkTrue(found, "节点: B 发现 A");
    if (found) {
        test::checkEquals(static_cast<int>(foundPeer.port), 9001, "节点: 公告端口正确");
        test::checkTrue(!foundPeer.address.empty(), "节点: 对端地址非空");
        test::checkEquals(static_cast<int>(nodeB.manager.peers().size()), 1, "节点: B 只记录一个节点");
    }

    nodeA.stop();
    nodeB.stop();
}

void peerExpires() {
    DiscoveryConfig configA;
    configA.announceIntervalMs = 100;
    configA.peerTimeoutMs = 2000;

    DiscoveryConfig configB;
    configB.announceIntervalMs = 100;
    configB.peerTimeoutMs = 400;

    NodeRuntime nodeA(0x11112222, 9101, configA);
    NodeRuntime nodeB(0x33334444, 9102, configB);

    test::checkTrue(nodeA.start(), "节点淘汰: A 启动");
    test::checkTrue(nodeB.start(), "节点淘汰: B 启动");

    bool found = false;
    for (int attempt = 0; attempt < 20 && !found; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        PeerInfo peer;
        found = nodeB.manager.findPeer(0x11112222, peer);
    }
    test::checkTrue(found, "节点淘汰: B 先发现 A");

    nodeA.stop();

    bool expired = false;
    for (int attempt = 0; attempt < 30 && !expired; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        PeerInfo peer;
        expired = !nodeB.manager.findPeer(0x11112222, peer);
    }
    test::checkTrue(expired, "节点淘汰: 停止公告后超时移除");

    nodeB.stop();
}

void queryInterface() {
    DiscoveryConfig config;
    NodeRuntime runtime(0x55556666, 9200, config);

    test::checkEquals(static_cast<int>(runtime.manager.nodeId()), 0x55556666, "节点: nodeId 访问器");
    test::checkTrue(runtime.manager.peers().empty(), "节点: 初始节点表为空");

    PeerInfo peer;
    test::checkFalse(runtime.manager.findPeer(0x99999999, peer), "节点: 未发现的节点查询失败");
}

} // namespace

void runNodeTests() {
    NETPROXY_CASE(twoNodesDiscover);
    NETPROXY_CASE(peerExpires);
    NETPROXY_CASE(queryInterface);
}
