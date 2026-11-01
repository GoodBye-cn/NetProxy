#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "TestSupport.h"
#include "core/IoLoop.h"
#include "node/NodeDiscovery.h"
#include "node/DiscoveryProtocol.h"

namespace {

using netproxy::core::IoLoop;
using netproxy::node::NodeDiscovery;
using netproxy::node::AnnounceMessage;
using netproxy::node::AnnounceType;
using netproxy::node::PeerInfo;

void messageRoundTrip() {
    AnnounceMessage original;
    original.type = AnnounceType::Announce;
    original.address = netproxy::net::IPAddress::parse("192.168.1.20", 0);
    original.nodeId = 0x12345678;
    original.port = 8080;

    const std::string encoded = original.encode();
    test::checkEquals(static_cast<int>(encoded.size()), 16, "节点报文: 固定 16 字节");

    AnnounceMessage decoded;
    test::checkTrue(AnnounceMessage::decode(encoded.data(), encoded.size(), decoded), "节点报文: 解码成功");
    test::checkTrue(decoded.type == AnnounceType::Announce, "节点报文: 类型");
    test::checkEquals(decoded.address.host(), std::string("192.168.1.20"), "节点报文: 地址");
    test::checkEquals(static_cast<int>(decoded.port), 8080, "节点报文: 端口");
    test::checkEquals(static_cast<int>(decoded.nodeId), 0x12345678, "节点报文: nodeId");
}

void invalidMessages() {
    AnnounceMessage message;
    test::checkFalse(AnnounceMessage::decode(nullptr, 0, message), "节点报文: 空报文");

    AnnounceMessage valid;
    valid.type = AnnounceType::Bye;
    valid.nodeId = 1;
    valid.port = 1000;
    const std::string bytes = valid.encode();

    test::checkFalse(AnnounceMessage::decode(bytes.data(), bytes.size() - 1, message), "节点报文: 长度不足");

    std::string broken = bytes;
    broken[0] = '\0';
    test::checkFalse(AnnounceMessage::decode(broken.data(), broken.size(), message), "节点报文: magic 错误");

    broken = bytes;
    broken[4] = '\x09';
    test::checkFalse(AnnounceMessage::decode(broken.data(), broken.size(), message), "节点报文: 版本错误");

    broken = bytes;
    broken[5] = '\x09';
    test::checkFalse(AnnounceMessage::decode(broken.data(), broken.size(), message), "节点报文: 类型错误");

    broken = bytes;
    broken[10] = '\0';
    broken[11] = '\0';
    test::checkFalse(AnnounceMessage::decode(broken.data(), broken.size(), message), "节点报文: 端口为 0");
}

void discoverNodes() {
    IoLoop ioA;
    IoLoop ioB;

    NodeDiscovery discoveryA(ioA, 0x11111111, "", 0);
    NodeDiscovery discoveryB(ioB, 0x22222222, "", 0);

    test::checkTrue(discoveryA.open(7001), "发现: A 打开通道");
    test::checkTrue(discoveryB.open(7002), "发现: B 打开通道");

    std::atomic<int> received{0};
    std::atomic<std::uint16_t> receivedPort{0};

    test::checkTrue(discoveryB.setReceiver([&received, &receivedPort](const PeerInfo& peer) {
        if (peer.nodeId == 0x11111111) {
            receivedPort = peer.port;
            received.fetch_add(1);
        }
    }),
                    "发现: B 注册回调");

    test::checkTrue(ioB.start(), "发现: B I/O 启动");

    test::checkTrue(discoveryA.announce(), "发现: A 公告");

    bool found = false;
    for (int attempt = 0; attempt < 20 && !found; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        found = received.load() > 0;
    }

    test::checkTrue(found, "发现: B 收到 A 的公告");
    if (found) {
        test::checkEquals(static_cast<int>(receivedPort.load()), 7001, "发现: 公告端口正确");
    }

    ioB.stop();
    discoveryA.close();
    discoveryB.close();
}

} // namespace

void runDiscoveryTests() {
    NETPROXY_CASE(messageRoundTrip);
    NETPROXY_CASE(invalidMessages);
    NETPROXY_CASE(discoverNodes);
}
