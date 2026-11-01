#include "PeerProtocol.h"

// 引入平台字节序转换函数（htonl / ntohs 等）
#include "Socket.h"

#include <cstring>

namespace netproxy::transport {

namespace {

bool isValidHelloType(std::uint8_t type) {
    return type == static_cast<std::uint8_t>(protocol::HelloType::Hello) ||
           type == static_cast<std::uint8_t>(protocol::HelloType::HelloAck) ||
           type == static_cast<std::uint8_t>(protocol::HelloType::HelloReject);
}

bool isValidFrameType(std::uint8_t type) {
    return type >= static_cast<std::uint8_t>(protocol::FrameType::Data) &&
           type <= static_cast<std::uint8_t>(protocol::FrameType::Close);
}

bool isStreamFrame(std::uint8_t type) {
    return type >= static_cast<std::uint8_t>(protocol::FrameType::StreamOpen) &&
           type <= static_cast<std::uint8_t>(protocol::FrameType::StreamClose);
}

void writeHeader(unsigned char* bytes, std::uint8_t type, std::uint32_t nodeId) {
    const std::uint32_t magic = htonl(protocol::MAGIC);
    std::memcpy(bytes, &magic, 4);
    bytes[4] = protocol::PEER_VERSION;
    bytes[5] = type;
    bytes[6] = 0;
    bytes[7] = 0;

    const std::uint32_t networkNodeId = htonl(nodeId);
    std::memcpy(bytes + 8, &networkNodeId, 4);
}

} // namespace

std::string PeerProtocol::encodeHello(const HelloMessage& message) {
    std::string buffer(HELLO_HEADER_SIZE, '\0');
    auto* bytes = reinterpret_cast<unsigned char*>(&buffer[0]);

    writeHeader(bytes, message.type, message.nodeId);

    const bool isReject = message.type == static_cast<std::uint8_t>(protocol::HelloType::HelloReject);
    if (isReject) {
        bytes[12] = message.reason;
        return buffer;
    }

    const std::uint32_t networkNonce = htonl(message.nonce);
    std::memcpy(bytes + 12, &networkNonce, 4);
    return buffer;
}

bool PeerProtocol::decodeHello(const void* data, std::size_t size, HelloMessage& message) {
    if (data == nullptr || size < HELLO_HEADER_SIZE) {
        return false;
    }

    const auto* bytes = static_cast<const unsigned char*>(data);

    std::uint32_t magic = 0;
    std::memcpy(&magic, bytes, 4);
    if (ntohl(magic) != protocol::MAGIC) {
        return false;
    }

    if (bytes[4] != protocol::PEER_VERSION) {
        return false;
    }

    const std::uint8_t type = bytes[5];
    if (!isValidHelloType(type)) {
        return false;
    }

    std::uint32_t networkNodeId = 0;
    std::memcpy(&networkNodeId, bytes + 8, 4);
    const std::uint32_t decodedNodeId = ntohl(networkNodeId);
    if (decodedNodeId == 0) {
        return false;
    }

    if (type == static_cast<std::uint8_t>(protocol::HelloType::HelloReject)) {
        message.type = type;
        message.nodeId = decodedNodeId;
        message.nonce = 0;
        message.reason = bytes[12];
        return true;
    }

    std::uint32_t networkNonce = 0;
    std::memcpy(&networkNonce, bytes + 12, 4);
    const std::uint32_t decodedNonce = ntohl(networkNonce);
    if (decodedNonce == 0) {
        return false;
    }

    message.type = type;
    message.nodeId = decodedNodeId;
    message.nonce = decodedNonce;
    message.reason = 0;
    return true;
}

std::string PeerProtocol::encodeFrame(const protocol::PeerFrame& frame) {
    const std::size_t size = FRAME_HEADER_SIZE + frame.payload.size();
    std::string buffer(size, '\0');
    auto* bytes = reinterpret_cast<unsigned char*>(&buffer[0]);

    writeHeader(bytes, frame.type, frame.nodeId);

    const std::uint32_t networkSeq = htonl(frame.connectionSeq);
    std::memcpy(bytes + 12, &networkSeq, 4);

    const std::uint32_t networkAck = htonl(frame.acknowledge);
    std::memcpy(bytes + 16, &networkAck, 4);

    const std::uint16_t networkWindow = htons(frame.window);
    std::memcpy(bytes + 20, &networkWindow, 2);

    const std::uint16_t networkLength = htons(static_cast<std::uint16_t>(frame.payload.size()));
    std::memcpy(bytes + 22, &networkLength, 2);

    const std::uint32_t networkStream = htonl(frame.streamId);
    std::memcpy(bytes + 24, &networkStream, 4);

    if (!frame.payload.empty()) {
        std::memcpy(bytes + FRAME_HEADER_SIZE, frame.payload.data(), frame.payload.size());
    }

    return buffer;
}

bool PeerProtocol::decodeFrame(const void* data, std::size_t size, protocol::PeerFrame& frame) {
    if (data == nullptr || size < FRAME_HEADER_SIZE) {
        return false;
    }

    const auto* bytes = static_cast<const unsigned char*>(data);

    std::uint32_t magic = 0;
    std::memcpy(&magic, bytes, 4);
    if (ntohl(magic) != protocol::MAGIC) {
        return false;
    }

    if (bytes[4] != protocol::PEER_VERSION) {
        return false;
    }

    const std::uint8_t type = bytes[5];
    if (!isValidFrameType(type)) {
        return false;
    }

    std::uint32_t networkNodeId = 0;
    std::memcpy(&networkNodeId, bytes + 8, 4);
    const std::uint32_t decodedNodeId = ntohl(networkNodeId);
    if (decodedNodeId == 0) {
        return false;
    }

    std::uint32_t networkSeq = 0;
    std::memcpy(&networkSeq, bytes + 12, 4);

    std::uint32_t networkAck = 0;
    std::memcpy(&networkAck, bytes + 16, 4);

    std::uint16_t networkWindow = 0;
    std::memcpy(&networkWindow, bytes + 20, 2);

    std::uint16_t networkLength = 0;
    std::memcpy(&networkLength, bytes + 22, 2);
    const std::uint16_t decodedLength = ntohs(networkLength);
    if (decodedLength != size - FRAME_HEADER_SIZE) {
        return false;
    }

    std::uint32_t networkStream = 0;
    std::memcpy(&networkStream, bytes + 24, 4);
    const std::uint32_t decodedStream = ntohl(networkStream);

    if (type == static_cast<std::uint8_t>(protocol::FrameType::Data)) {
        if (decodedStream == 0 || decodedLength == 0) {
            return false;
        }
    } else if (type == static_cast<std::uint8_t>(protocol::FrameType::Ping) ||
               type == static_cast<std::uint8_t>(protocol::FrameType::Close)) {
        if (decodedStream != 0) {
            return false;
        }
    } else if (isStreamFrame(type)) {
        if (decodedStream == 0) {
            return false;
        }
    }

    frame.type = type;
    frame.nodeId = decodedNodeId;
    frame.connectionSeq = ntohl(networkSeq);
    frame.acknowledge = ntohl(networkAck);
    frame.window = ntohs(networkWindow);
    frame.payloadLength = decodedLength;
    frame.streamId = decodedStream;
    frame.payload.assign(reinterpret_cast<const char*>(bytes + FRAME_HEADER_SIZE), decodedLength);
    return true;
}

} // namespace netproxy::transport
