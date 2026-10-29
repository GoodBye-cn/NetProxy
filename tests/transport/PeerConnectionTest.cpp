#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "TestSupport.h"
#include "net/Socket.h"
#include "transport/PeerConnection.h"

namespace {

using netproxy::net::IPAddress;
using netproxy::protocol::FrameType;
using netproxy::protocol::HelloType;
using netproxy::protocol::Reason;
using netproxy::protocol::StreamEvent;
using netproxy::transport::ConnectionState;
using netproxy::transport::HelloMessage;
using netproxy::transport::PeerConfig;
using netproxy::transport::PeerConnection;
using netproxy::transport::PeerFrame;
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

void acceptStream(PeerConnection& connection, std::uint32_t streamId, std::uint32_t seq) {
    PeerFrame accept;
    accept.type = static_cast<std::uint8_t>(FrameType::StreamAccept);
    accept.nodeId = 0x22;
    accept.connectionSeq = seq;
    accept.acknowledge = seq + 1;
    accept.window = 8;
    accept.streamId = streamId;
    connection.onFrame(accept);
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

    connection.open();

    HelloMessage reject;
    reject.type = static_cast<std::uint8_t>(HelloType::HelloReject);
    reject.nodeId = 0x22;
    reject.reason = 1;
    connection.onHelloReject(reject);

    test::checkTrue(connection.state() == ConnectionState::Closed, "连接: 被拒绝后关闭");
}

void openStreamFlow() {
    Capture capture;
    PeerConnection connection(0x22, 0x11, IPAddress::parse("127.0.0.1", 9000), fastConfig(), capture.sender());
    completeHandshake(capture, connection);

    std::atomic<int> opened{0};
    connection.setStreamHandler([&opened](std::uint32_t, StreamEvent event, Reason) {
        if (event == StreamEvent::Opened) {
            opened.fetch_add(1);
        }
    });

    test::checkTrue(connection.openStream(1), "流: 打开请求成功");
    test::checkTrue(!connection.openStream(1), "流: 重复打开被拒绝");

    PeerFrame frame;
    test::checkTrue(capture.lastFrame(frame), "流: 捕获 StreamOpen");
    test::checkTrue(frame.type == static_cast<std::uint8_t>(FrameType::StreamOpen), "流: 帧类型为 StreamOpen");

    acceptStream(connection, 1, 0);
    test::checkEquals(opened.load(), 1, "流: 回调 Opened");
}

void dataDelivery() {
    Capture capture;
    PeerConnection connection(0x22, 0x11, IPAddress::parse("127.0.0.1", 9000), fastConfig(), capture.sender());
    completeHandshake(capture, connection);
    connection.openStream(1);
    acceptStream(connection, 1, 0);

    std::string received;
    std::uint32_t receivedStream = 0;
    connection.setDataHandler([&received, &receivedStream](std::uint32_t streamId, const char* data, std::size_t size) {
        receivedStream = streamId;
        received.assign(data, size);
    });

    PeerFrame data;
    data.type = static_cast<std::uint8_t>(FrameType::Data);
    data.nodeId = 0x22;
    data.connectionSeq = 1;
    data.acknowledge = 1;
    data.streamId = 1;
    data.payload = "hello";
    connection.onFrame(data);

    test::checkEquals(received, std::string("hello"), "数据: 按序交付");
    test::checkEquals(static_cast<int>(receivedStream), 1, "数据: 流标识正确");
}

void windowLimitsSending() {
    Capture capture;
    PeerConfig config = fastConfig();
    config.sendWindow = 4;
    PeerConnection connection(0x22, 0x11, IPAddress::parse("127.0.0.1", 9000), config, capture.sender());
    completeHandshake(capture, connection);
    connection.openStream(1);
    acceptStream(connection, 1, 0);

    const std::size_t before = capture.countFrames(FrameType::Data);
    for (int index = 0; index < 10; ++index) {
        test::checkTrue(connection.sendData(1, "x", 1), "窗口: 提交成功");
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
    connection.openStream(1);
    acceptStream(connection, 1, 0);

    test::checkTrue(connection.openStream(2), "重传: 准备第二条流");
    const std::size_t before = capture.packets.size();

    connection.tick(Clock::now() + std::chrono::milliseconds(20));
    test::checkTrue(capture.packets.size() > before, "重传: 超时后重发队首");
}

void streamCloseCallback() {
    Capture capture;
    PeerConnection connection(0x22, 0x11, IPAddress::parse("127.0.0.1", 9000), fastConfig(), capture.sender());
    completeHandshake(capture, connection);
    connection.openStream(1);
    acceptStream(connection, 1, 0);

    std::atomic<int> closed{0};
    connection.setStreamHandler([&closed](std::uint32_t, StreamEvent event, Reason) {
        if (event == StreamEvent::Closed) {
            closed.fetch_add(1);
        }
    });

    test::checkTrue(connection.closeStream(1), "关闭: 流关闭成功");
    test::checkTrue(!connection.closeStream(1), "关闭: 重复关闭返回失败");
    test::checkEquals(closed.load(), 1, "关闭: 回调 Closed");
}

} // namespace

void runPeerConnectionTests() {
    NETPROXY_CASE(helloRetriesAndTimeout);
    NETPROXY_CASE(helloAckConnects);
    NETPROXY_CASE(helloRejectCloses);
    NETPROXY_CASE(openStreamFlow);
    NETPROXY_CASE(dataDelivery);
    NETPROXY_CASE(windowLimitsSending);
    NETPROXY_CASE(retransmitOnTimeout);
    NETPROXY_CASE(streamCloseCallback);
}
