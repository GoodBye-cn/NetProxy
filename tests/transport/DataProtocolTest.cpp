#include <string>

#include "TestSupport.h"
#include "transport/DataProtocol.h"

namespace {

using netproxy::transport::DataMessage;

void dataRoundTrip() {
    DataMessage original;
    original.type = 6;
    original.sessionId = 0x12345678;
    original.nodeId = 0x0000000A;
    original.sequence = 7;
    original.acknowledge = 3;
    original.window = 16;
    original.payload = "hello";

    const std::string encoded = original.encodePacket();
    test::checkEquals(static_cast<int>(encoded.size()), 33, "数据报文: 头部 28 加负载 5");

    DataMessage decoded;
    test::checkTrue(DataMessage::decodePacket(encoded.data(), encoded.size(), decoded), "数据报文: 解码成功");
    test::checkEquals(static_cast<int>(decoded.sessionId), 0x12345678, "数据报文: sessionId");
    test::checkEquals(static_cast<int>(decoded.nodeId), 0x0000000A, "数据报文: nodeId");
    test::checkEquals(static_cast<int>(decoded.sequence), 7, "数据报文: sequence");
    test::checkEquals(static_cast<int>(decoded.acknowledge), 3, "数据报文: acknowledge");
    test::checkEquals(static_cast<int>(decoded.window), 16, "数据报文: window");
    test::checkEquals(decoded.payload, std::string("hello"), "数据报文: 负载");
}

void ackRoundTrip() {
    DataMessage original;
    original.type = 7;
    original.sessionId = 0x12345678;
    original.nodeId = 0x0000000A;
    original.acknowledge = 5;
    original.window = 8;

    const std::string encoded = original.encodePacket();
    test::checkEquals(static_cast<int>(encoded.size()), 24, "Ack: 固定 24 字节");

    DataMessage decoded;
    test::checkTrue(DataMessage::decodePacket(encoded.data(), encoded.size(), decoded), "Ack: 解码成功");
    test::checkEquals(static_cast<int>(decoded.acknowledge), 5, "Ack: acknowledge");
    test::checkEquals(static_cast<int>(decoded.window), 8, "Ack: window");
}

void invalidDataMessages() {
    DataMessage message;
    test::checkFalse(DataMessage::decodePacket(nullptr, 0, message), "数据报文: 空报文");

    DataMessage valid;
    valid.type = 6;
    valid.sessionId = 1;
    valid.nodeId = 1;
    valid.sequence = 0;
    valid.payload = "x";
    const std::string bytes = valid.encodePacket();

    test::checkFalse(DataMessage::decodePacket(bytes.data(), bytes.size() - 1, message), "数据报文: 长度不足");

    std::string broken = bytes;
    broken[0] = '\0';
    test::checkFalse(DataMessage::decodePacket(broken.data(), broken.size(), message), "数据报文: magic 错误");

    broken = bytes;
    broken[8] = '\0';
    broken[9] = '\0';
    broken[10] = '\0';
    broken[11] = '\0';
    test::checkFalse(DataMessage::decodePacket(broken.data(), broken.size(), message), "数据报文: nodeId 为 0");

    broken = bytes;
    broken[26] = '\0';
    broken[27] = '\0';
    test::checkFalse(DataMessage::decodePacket(broken.data(), broken.size(), message), "数据报文: 负载长度为 0");
}

} // namespace

void runTransportTests() {
    NETPROXY_CASE(dataRoundTrip);
    NETPROXY_CASE(ackRoundTrip);
    NETPROXY_CASE(invalidDataMessages);
}
