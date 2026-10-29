#include "DataProtocol.h"

#include "protocol/Protocol.h"

// 引入平台字节序转换函数（htonl / ntohs 等）
#include "net/Socket.h"

#include <cstring>

namespace netproxy::transport {

namespace {

bool isValidType(std::uint8_t type) {
    return type == static_cast<std::uint8_t>(protocol::MessageType::Data) ||
           type == static_cast<std::uint8_t>(protocol::MessageType::Ack);
}

} // namespace

std::string DataMessage::encodePacket() const {
    const bool isData = type == static_cast<std::uint8_t>(protocol::MessageType::Data);
    const std::size_t size = isData ? DATA_HEADER_SIZE + payload.size() : ACK_HEADER_SIZE;

    std::string buffer(size, '\0');
    auto* bytes = reinterpret_cast<unsigned char*>(&buffer[0]);

    const std::uint32_t magic = htonl(protocol::MAGIC);
    std::memcpy(bytes, &magic, 4);
    bytes[4] = protocol::VERSION;
    bytes[5] = type;
    bytes[6] = 0;
    bytes[7] = 0;

    const std::uint32_t networkNodeId = htonl(nodeId);
    std::memcpy(bytes + 8, &networkNodeId, 4);

    const std::uint32_t networkSessionId = htonl(sessionId);
    std::memcpy(bytes + 12, &networkSessionId, 4);

    if (isData) {
        const std::uint32_t networkSequence = htonl(sequence);
        std::memcpy(bytes + 16, &networkSequence, 4);

        const std::uint32_t networkAck = htonl(acknowledge);
        std::memcpy(bytes + 20, &networkAck, 4);

        const std::uint16_t networkWindow = htons(window);
        std::memcpy(bytes + 24, &networkWindow, 2);

        const std::uint16_t networkLength = htons(static_cast<std::uint16_t>(payload.size()));
        std::memcpy(bytes + 26, &networkLength, 2);

        if (!payload.empty()) {
            std::memcpy(bytes + DATA_HEADER_SIZE, payload.data(), payload.size());
        }
    } else {
        const std::uint32_t networkAck = htonl(acknowledge);
        std::memcpy(bytes + 16, &networkAck, 4);

        const std::uint16_t networkWindow = htons(window);
        std::memcpy(bytes + 20, &networkWindow, 2);
    }

    return buffer;
}

bool DataMessage::decodePacket(const void* data, std::size_t size, DataMessage& message) {
    if (data == nullptr || size < protocol::HEADER_SIZE) {
        return false;
    }

    const auto* bytes = static_cast<const unsigned char*>(data);

    std::uint32_t magic = 0;
    std::memcpy(&magic, bytes, 4);
    if (ntohl(magic) != protocol::MAGIC) {
        return false;
    }

    if (bytes[4] != protocol::VERSION) {
        return false;
    }

    const std::uint8_t type = bytes[5];
    if (!isValidType(type)) {
        return false;
    }

    std::uint32_t networkNodeId = 0;
    std::memcpy(&networkNodeId, bytes + 8, 4);
    const std::uint32_t decodedNodeId = ntohl(networkNodeId);
    if (decodedNodeId == 0) {
        return false;
    }

    std::uint32_t networkSessionId = 0;
    std::memcpy(&networkSessionId, bytes + 12, 4);
    const std::uint32_t decodedSessionId = ntohl(networkSessionId);
    if (decodedSessionId == 0) {
        return false;
    }

    message.type = type;
    message.sessionId = decodedSessionId;
    message.nodeId = decodedNodeId;
    message.sequence = 0;
    message.acknowledge = 0;
    message.window = 0;
    message.payloadLength = 0;
    message.payload.clear();

    if (type == static_cast<std::uint8_t>(protocol::MessageType::Data)) {
        if (size < DATA_HEADER_SIZE) {
            return false;
        }

        std::uint32_t networkSequence = 0;
        std::memcpy(&networkSequence, bytes + 16, 4);

        std::uint32_t networkAck = 0;
        std::memcpy(&networkAck, bytes + 20, 4);

        std::uint16_t networkWindow = 0;
        std::memcpy(&networkWindow, bytes + 24, 2);

        std::uint16_t networkLength = 0;
        std::memcpy(&networkLength, bytes + 26, 2);
        const std::uint16_t decodedLength = ntohs(networkLength);
        if (decodedLength == 0 || decodedLength > size - DATA_HEADER_SIZE) {
            return false;
        }

        message.sequence = ntohl(networkSequence);
        message.acknowledge = ntohl(networkAck);
        message.window = ntohs(networkWindow);
        message.payloadLength = decodedLength;
        message.payload.assign(reinterpret_cast<const char*>(bytes + DATA_HEADER_SIZE), decodedLength);
        return true;
    }

    if (size < ACK_HEADER_SIZE) {
        return false;
    }

    std::uint32_t networkAck = 0;
    std::memcpy(&networkAck, bytes + 16, 4);

    std::uint16_t networkWindow = 0;
    std::memcpy(&networkWindow, bytes + 20, 2);

    message.acknowledge = ntohl(networkAck);
    message.window = ntohs(networkWindow);
    return true;
}

} // namespace netproxy::transport
