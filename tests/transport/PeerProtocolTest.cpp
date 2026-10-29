#include <string>

#include "TestSupport.h"
#include "transport/PeerProtocol.h"

namespace {

using netproxy::protocol::FrameType;
using netproxy::protocol::HelloType;
using netproxy::transport::HelloMessage;
using netproxy::transport::PeerFrame;
using netproxy::transport::PeerProtocol;

void helloRoundTrip() {
    HelloMessage original;
    original.type = static_cast<std::uint8_t>(HelloType::Hello);
    original.nodeId = 0x12345678;
    original.nonce = 0xDEADBEEF;

    const std::string encoded = PeerProtocol::encodeHello(original);
    test::checkEquals(static_cast<int>(encoded.size()), 16, "握手: 固定 16 字节");

    HelloMessage decoded;
    test::checkTrue(PeerProtocol::decodeHello(encoded.data(), encoded.size(), decoded), "握手: 解码成功");
    test::checkEquals(static_cast<int>(decoded.nodeId), 0x12345678, "握手: nodeId");
    test::checkEquals(static_cast<int>(decoded.nonce), 0xDEADBEEF, "握手: nonce");
    test::checkTrue(decoded.type == static_cast<std::uint8_t>(HelloType::Hello), "握手: 类型");

    HelloMessage ack;
    ack.type = static_cast<std::uint8_t>(HelloType::HelloAck);
    ack.nodeId = 0x0A0B0C0D;
    ack.nonce = 0xDEADBEEF;
    const std::string ackBytes = PeerProtocol::encodeHello(ack);
    test::checkTrue(PeerProtocol::decodeHello(ackBytes.data(), ackBytes.size(), decoded), "握手: Ack 解码");
    test::checkEquals(static_cast<int>(decoded.nodeId), 0x0A0B0C0D, "握手: Ack nodeId");
}

void helloReject() {
    HelloMessage original;
    original.type = static_cast<std::uint8_t>(HelloType::HelloReject);
    original.nodeId = 0x12345678;
    original.reason = 1;

    const std::string encoded = PeerProtocol::encodeHello(original);
    HelloMessage decoded;
    test::checkTrue(PeerProtocol::decodeHello(encoded.data(), encoded.size(), decoded), "握手: 拒绝解码");
    test::checkEquals(static_cast<int>(decoded.reason), 1, "握手: 拒绝原因");
}

void frameRoundTrip() {
    PeerFrame original;
    original.type = static_cast<std::uint8_t>(FrameType::Data);
    original.nodeId = 0x0000000A;
    original.connectionSeq = 7;
    original.acknowledge = 3;
    original.window = 16;
    original.streamId = 0x12345678;
    original.payload = "hello";

    const std::string encoded = PeerProtocol::encodeFrame(original);
    test::checkEquals(static_cast<int>(encoded.size()), 33, "连接帧: 28 + 负载");

    PeerFrame decoded;
    test::checkTrue(PeerProtocol::decodeFrame(encoded.data(), encoded.size(), decoded), "连接帧: 解码成功");
    test::checkEquals(static_cast<int>(decoded.nodeId), 0x0000000A, "连接帧: nodeId");
    test::checkEquals(static_cast<int>(decoded.connectionSeq), 7, "连接帧: 序号");
    test::checkEquals(static_cast<int>(decoded.acknowledge), 3, "连接帧: 确认");
    test::checkEquals(static_cast<int>(decoded.window), 16, "连接帧: 窗口");
    test::checkEquals(static_cast<int>(decoded.streamId), 0x12345678, "连接帧: streamId");
    test::checkEquals(decoded.payload, std::string("hello"), "连接帧: 负载");
}

void controlFrameRoundTrip() {
    PeerFrame close;
    close.type = static_cast<std::uint8_t>(FrameType::Close);
    close.nodeId = 0x0000000A;
    close.connectionSeq = 9;
    close.payload.assign(1, static_cast<char>(3));

    const std::string closeBytes = PeerProtocol::encodeFrame(close);
    test::checkEquals(static_cast<int>(closeBytes.size()), 29, "控制帧: 28 + 1 字节原因");

    PeerFrame decoded;
    test::checkTrue(PeerProtocol::decodeFrame(closeBytes.data(), closeBytes.size(), decoded), "控制帧: 解码");
    test::checkEquals(static_cast<int>(decoded.streamId), 0, "控制帧: streamId 为 0");

    PeerFrame ping;
    ping.type = static_cast<std::uint8_t>(FrameType::Ping);
    ping.nodeId = 0x0000000A;
    const std::string pingBytes = PeerProtocol::encodeFrame(ping);
    test::checkEquals(static_cast<int>(pingBytes.size()), 28, "Ping: 无负载");
    test::checkTrue(PeerProtocol::decodeFrame(pingBytes.data(), pingBytes.size(), decoded), "Ping: 解码");
}

void invalidHello() {
    HelloMessage message;
    test::checkFalse(PeerProtocol::decodeHello(nullptr, 0, message), "握手: 空报文");

    HelloMessage valid;
    valid.nodeId = 1;
    valid.nonce = 1;
    const std::string bytes = PeerProtocol::encodeHello(valid);

    test::checkFalse(PeerProtocol::decodeHello(bytes.data(), bytes.size() - 1, message), "握手: 长度不足");

    std::string broken = bytes;
    broken[0] = '\0';
    test::checkFalse(PeerProtocol::decodeHello(broken.data(), broken.size(), message), "握手: magic 错误");

    broken = bytes;
    broken[4] = 1;
    test::checkFalse(PeerProtocol::decodeHello(broken.data(), broken.size(), message), "握手: 版本错误");

    broken = bytes;
    broken[5] = 9;
    test::checkFalse(PeerProtocol::decodeHello(broken.data(), broken.size(), message), "握手: 类型错误");

    broken = bytes;
    broken[8] = '\0';
    broken[9] = '\0';
    broken[10] = '\0';
    broken[11] = '\0';
    test::checkFalse(PeerProtocol::decodeHello(broken.data(), broken.size(), message), "握手: nodeId 为 0");

    broken = bytes;
    broken[12] = '\0';
    broken[13] = '\0';
    broken[14] = '\0';
    broken[15] = '\0';
    test::checkFalse(PeerProtocol::decodeHello(broken.data(), broken.size(), message), "握手: nonce 为 0");
}

void invalidFrame() {
    PeerFrame frame;
    test::checkFalse(PeerProtocol::decodeFrame(nullptr, 0, frame), "连接帧: 空报文");

    PeerFrame valid;
    valid.type = static_cast<std::uint8_t>(FrameType::Data);
    valid.nodeId = 1;
    valid.streamId = 2;
    valid.connectionSeq = 0;
    valid.payload = "x";
    const std::string bytes = PeerProtocol::encodeFrame(valid);

    test::checkFalse(PeerProtocol::decodeFrame(bytes.data(), bytes.size() - 1, frame), "连接帧: 长度不足");

    std::string broken = bytes;
    broken[0] = '\0';
    test::checkFalse(PeerProtocol::decodeFrame(broken.data(), broken.size(), frame), "连接帧: magic 错误");

    broken = bytes;
    broken[4] = 1;
    test::checkFalse(PeerProtocol::decodeFrame(broken.data(), broken.size(), frame), "连接帧: 版本错误");

    broken = bytes;
    broken[5] = 9;
    test::checkFalse(PeerProtocol::decodeFrame(broken.data(), broken.size(), frame), "连接帧: 类型错误");

    broken = bytes;
    broken[8] = '\0';
    broken[9] = '\0';
    broken[10] = '\0';
    broken[11] = '\0';
    test::checkFalse(PeerProtocol::decodeFrame(broken.data(), broken.size(), frame), "连接帧: nodeId 为 0");

    broken = bytes;
    broken[24] = '\0';
    broken[25] = '\0';
    broken[26] = '\0';
    broken[27] = '\0';
    test::checkFalse(PeerProtocol::decodeFrame(broken.data(), broken.size(), frame), "连接帧: Data 的 streamId 为 0");
}

} // namespace

void runPeerProtocolTests() {
    NETPROXY_CASE(helloRoundTrip);
    NETPROXY_CASE(helloReject);
    NETPROXY_CASE(frameRoundTrip);
    NETPROXY_CASE(controlFrameRoundTrip);
    NETPROXY_CASE(invalidHello);
    NETPROXY_CASE(invalidFrame);
}
