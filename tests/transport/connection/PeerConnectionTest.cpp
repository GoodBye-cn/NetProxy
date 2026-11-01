#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "net/Socket.h"
#include "transport/connection/PeerConnection.h"

namespace {

using netproxy::net::IPAddress;
using netproxy::protocol::FrameType;
using netproxy::protocol::HelloType;
using netproxy::protocol::Reason;
using netproxy::transport::ConnectionState;
using netproxy::transport::HelloMessage;
using netproxy::transport::PeerConfig;
using netproxy::transport::PeerConnection;
using netproxy::protocol::PeerFrame;
using netproxy::transport::PeerProtocol;

using Clock = std::chrono::steady_clock;

// 捕获发出的报文，便于断言与喂回。
struct Capture {
    std::vector<std::string> packets;

    PeerConnection::Sender sender() {
        return [this](const std::string& packet, const IPAddress&) { packets.push_back(packet); };
    }

    bool lastHello(HelloMessage& message) const {
        if (packets.empty()) {
            return false;
        }
        return PeerProtocol::decodeHello(packets.back().data(), packets.back().size(), message);
    }

    bool lastFrame(PeerFrame& frame) const {
        if (packets.empty()) {
            return false;
        }
        return PeerProtocol::decodeFrame(packets.back().data(), packets.back().size(), frame);
    }

    std::size_t countFrames(FrameType type) const {
        std::size_t count = 0;
        for (const std::string& packet : packets) {
            PeerFrame frame;
            if (PeerProtocol::decodeFrame(packet.data(), packet.size(), frame) &&
                frame.type == static_cast<std::uint8_t>(type)) {
                ++count;
            }
        }
        return count;
    }
};

PeerConfig fastConfig() {
    PeerConfig config;
    config.helloRetryIntervalMs = 10;
    config.helloRetryCount = 2;
    config.retransmitTimeoutMs = 10;
    config.maxRetransmitCount = 2;
    config.pingIntervalMs = 100000;
    config.pingTimeoutMs = 100000;
    config.idleTimeoutMs = 100000;
    config.sendWindow = 4;
    config.receiveWindow = 8;
    return config;
}

void completeHandshake(Capture& capture, PeerConnection& connection) {
    connection.open();

    HelloMessage hello;
    capture.lastHello(hello);

    HelloMessage ack;
    ack.type = static_cast<std::uint8_t>(HelloType::HelloAck);
    ack.nodeId = 0x22;
    ack.nonce = hello.nonce;
    connection.onHelloAck(ack);
}

PeerFrame dataFrame(std::uint32_t seq, const std::string& payload) {
    PeerFrame frame;
    frame.type = static_cast<std::uint8_t>(FrameType::Data);
    frame.nodeId = 0x22;
    frame.connectionSeq = seq;
    frame.acknowledge = seq + 1;
    frame.window = 8;
    frame.streamId = 1;
    frame.payload = payload;
    return frame;
}

void helloRetriesAndTimeout() {
    Capture capture;
    PeerConnection connection(0x22, 0x11, IPAddress::parse("127.0.0.1", 9000), fastConfig(), capture.sender());

    test::checkTrue(connection.open(), "连接: 发起成功");
    test::checkEquals(static_cast<int>(capture.packets.size()), 1, "连接: 首次 Hello");

    const auto base = Clock::now() + std::chrono::milliseconds(1000);
    connection.tick(base);
    test::checkEquals(static_cast<int>(capture.packets.size()), 2, "连接: 第一次重发");

    connection.tick(base + std::chrono::milliseconds(1000));
    test::checkEquals(static_cast<int>(capture.packets.size()), 3, "连接: 第二次重发");

    connection.tick(base + std::chrono::milliseconds(2000));
    test::checkTrue(connection.state() == ConnectionState::Closed, "连接: 重试耗尽后关闭");
}

void helloAckConnects() {
    Capture capture;
    PeerConnection connection(0x22, 0x11, IPAddress::parse("127.0.0.1", 9000), fastConfig(), capture.sender());

    completeHandshake(capture, connection);
    test::checkTrue(connection.isConnected(), "连接: 收到 HelloAck 后建立");
}

void helloRejectCloses() {
    Capture capture;
    PeerConnection connection(0x22, 0x11, IPAddress::parse("127.0.0.1", 9000), fastConfig(), capture.sender());

    std::atomic<int> closed{0};
    connection.setCloseHandler([&closed](Reason) { closed.fetch_add(1); });

    connection.open();

    HelloMessage reject;
    reject.type = static_cast<std::uint8_t>(HelloType::HelloReject);
    reject.nodeId = 0x22;
    reject.reason = 1;
    connection.onHelloReject(reject);

    test::checkTrue(connection.state() == ConnectionState::Closed, "连接: 被拒绝后关闭");
    test::checkEquals(closed.load(), 1, "连接: 被拒绝后回调 CloseHandler");
}

void orderedDelivery() {
    Capture capture;
    PeerConnection connection(0x22, 0x11, IPAddress::parse("127.0.0.1", 9000), fastConfig(), capture.sender());
    completeHandshake(capture, connection);

    std::vector<std::string> delivered;
    connection.setFrameHandler([&delivered](const PeerFrame& frame) { delivered.push_back(frame.payload); });

    // 先到 seq=1，再到 seq=0：按序交付应是 one、two。
    connection.onFrame(dataFrame(1, "two"));
    test::checkEquals(static_cast<int>(delivered.size()), 0, "有序: 乱序帧被缓存");

    connection.onFrame(dataFrame(0, "one"));
    test::checkEquals(static_cast<int>(delivered.size()), 2, "有序: 两帧均已交付");

    if (delivered.size() == 2) {
        test::checkEquals(delivered[0], std::string("one"), "有序: 第一帧是 one");
        test::checkEquals(delivered[1], std::string("two"), "有序: 第二帧是 two");
    }
}

void duplicateFrameDropped() {
    Capture capture;
    PeerConnection connection(0x22, 0x11, IPAddress::parse("127.0.0.1", 9000), fastConfig(), capture.sender());
    completeHandshake(capture, connection);

    std::atomic<int> delivered{0};
    connection.setFrameHandler([&delivered](const PeerFrame&) { delivered.fetch_add(1); });

    connection.onFrame(dataFrame(0, "first"));
    connection.onFrame(dataFrame(0, "first"));
    test::checkEquals(delivered.load(), 1, "去重: 重复帧不交付");

    connection.onFrame(dataFrame(1, "second"));
    test::checkEquals(delivered.load(), 2, "去重: 后续序号正常交付");
}

void windowLimitsSending() {
    Capture capture;
    PeerConfig config = fastConfig();
    config.sendWindow = 4;
    PeerConnection connection(0x22, 0x11, IPAddress::parse("127.0.0.1", 9000), config, capture.sender());
    completeHandshake(capture, connection);

    const std::size_t before = capture.countFrames(FrameType::Data);
    for (int index = 0; index < 10; ++index) {
        PeerFrame frame;
        frame.type = static_cast<std::uint8_t>(FrameType::Data);
        frame.streamId = 1;
        frame.payload = "x";
        test::checkTrue(connection.send(std::move(frame)), "窗口: 提交成功");
    }
    const std::size_t sent = capture.countFrames(FrameType::Data) - before;
    test::checkEquals(static_cast<int>(sent), 4, "窗口: 只发出窗口内的帧");

    PeerFrame ackFrame;
    ackFrame.type = static_cast<std::uint8_t>(FrameType::Ping);
    ackFrame.nodeId = 0x22;
    ackFrame.acknowledge = 4;
    ackFrame.window = 8;
    connection.onFrame(ackFrame);

    const std::size_t after = capture.countFrames(FrameType::Data) - before;
    test::checkTrue(after > sent, "窗口: 确认后继续发送");
}

void retransmitOnTimeout() {
    Capture capture;
    PeerConnection connection(0x22, 0x11, IPAddress::parse("127.0.0.1", 9000), fastConfig(), capture.sender());
    completeHandshake(capture, connection);

    PeerFrame frame;
    frame.type = static_cast<std::uint8_t>(FrameType::Data);
    frame.streamId = 1;
    frame.payload = "x";
    test::checkTrue(connection.send(std::move(frame)), "重传: 提交成功");

    const std::size_t before = capture.packets.size();
    connection.tick(Clock::now() + std::chrono::milliseconds(20));
    test::checkTrue(capture.packets.size() > before, "重传: 超时后重发队首");
}

void payloadLimitRejected() {
    Capture capture;
    PeerConfig config = fastConfig();
    config.maxPayloadSize = 8;
    PeerConnection connection(0x22, 0x11, IPAddress::parse("127.0.0.1", 9000), config, capture.sender());
    completeHandshake(capture, connection);

    PeerFrame tooLarge;
    tooLarge.type = static_cast<std::uint8_t>(FrameType::Data);
    tooLarge.streamId = 1;
    tooLarge.payload.assign(9, 'x');
    test::checkFalse(connection.send(std::move(tooLarge)), "长度: 超过 maxPayloadSize 被拒绝");

    PeerFrame exactly;
    exactly.type = static_cast<std::uint8_t>(FrameType::Data);
    exactly.streamId = 1;
    exactly.payload.assign(8, 'x');
    test::checkTrue(connection.send(std::move(exactly)), "长度: 等于 maxPayloadSize 被接受");
}

void sendRejectedWhenNotConnected() {
    Capture capture;
    PeerConnection connection(0x22, 0x11, IPAddress::parse("127.0.0.1", 9000), fastConfig(), capture.sender());

    PeerFrame frame;
    frame.type = static_cast<std::uint8_t>(FrameType::Data);
    frame.streamId = 1;
    frame.payload = "x";
    test::checkFalse(connection.send(std::move(frame)), "状态: 未连接时拒绝发送");
}

void closeHandlerFiresOnce() {
    Capture capture;
    PeerConnection connection(0x22, 0x11, IPAddress::parse("127.0.0.1", 9000), fastConfig(), capture.sender());
    completeHandshake(capture, connection);

    std::atomic<int> closed{0};
    connection.setCloseHandler([&closed](Reason) { closed.fetch_add(1); });

    connection.close(Reason::Local);
    connection.close(Reason::Local);

    test::checkTrue(connection.state() == ConnectionState::Closed, "关闭: 状态为 Closed");
    test::checkEquals(closed.load(), 1, "关闭: CloseHandler 只触发一次");
}

void idleCloseUsesStreams() {
    Capture capture;
    PeerConfig config = fastConfig();
    config.idleTimeoutMs = 50;

    bool hasStreams = true;
    const IPAddress address = IPAddress::parse("127.0.0.1", 9000);
    PeerConnection connection(0x22, 0x11, address, config, capture.sender(), [&hasStreams] { return hasStreams; });
    completeHandshake(capture, connection);

    const auto now = Clock::now();
    connection.tick(now + std::chrono::milliseconds(200));
    test::checkTrue(connection.isConnected(), "空闲: 仍有流时不关闭");

    hasStreams = false;
    connection.tick(now + std::chrono::milliseconds(400));
    test::checkTrue(connection.state() == ConnectionState::Closing, "空闲: 无流且超时后进入 Closing");
}

void remoteCloseNotifiesHandler() {
    Capture capture;
    PeerConnection connection(0x22, 0x11, IPAddress::parse("127.0.0.1", 9000), fastConfig(), capture.sender());
    completeHandshake(capture, connection);

    std::atomic<int> closed{0};
    Reason observed = Reason::Local;
    connection.setCloseHandler([&closed, &observed](Reason reason) {
        observed = reason;
        closed.fetch_add(1);
    });

    PeerFrame closeFrame;
    closeFrame.type = static_cast<std::uint8_t>(FrameType::Close);
    closeFrame.nodeId = 0x22;
    closeFrame.payload = std::string(1, static_cast<char>(Reason::Remote));
    connection.onFrame(closeFrame);

    test::checkTrue(connection.state() == ConnectionState::Closed, "远端关闭: 状态为 Closed");
    test::checkEquals(closed.load(), 1, "远端关闭: 回调一次");
    test::checkTrue(observed == Reason::Remote, "远端关闭: 原因为 Remote");
}

} // namespace

void runPeerConnectionTests() {
    NETPROXY_CASE(helloRetriesAndTimeout);
    NETPROXY_CASE(helloAckConnects);
    NETPROXY_CASE(helloRejectCloses);
    NETPROXY_CASE(orderedDelivery);
    NETPROXY_CASE(duplicateFrameDropped);
    NETPROXY_CASE(windowLimitsSending);
    NETPROXY_CASE(retransmitOnTimeout);
    NETPROXY_CASE(payloadLimitRejected);
    NETPROXY_CASE(sendRejectedWhenNotConnected);
    NETPROXY_CASE(closeHandlerFiresOnce);
    NETPROXY_CASE(idleCloseUsesStreams);
    NETPROXY_CASE(remoteCloseNotifiesHandler);
}
