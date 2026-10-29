#include "SessionProtocol.h"

#include <cstring>

// 引入平台字节序转换函数（htonl / ntohs 等）
#include "net/Socket.h"

namespace netproxy::session {

namespace {

bool isValidType(std::uint8_t rawType) {
    return rawType >= static_cast<std::uint8_t>(SessionType::Open) &&
           rawType <= static_cast<std::uint8_t>(SessionType::Ping);
}

} // namespace

std::size_t SessionMessage::encodedSize() const {
    switch (type) {
    case SessionType::Open:
        return 24;
    case SessionType::Accept:
    case SessionType::Reject:
    case SessionType::Close:
        return 20;
    case SessionType::Ping:
        return 16;
    }
    return HEADER_SIZE;
}

std::string SessionMessage::encode() const {
    const std::size_t size = encodedSize();
    std::string buffer(size, '\0');
    auto* bytes = reinterpret_cast<unsigned char*>(&buffer[0]);

    const std::uint32_t magic = htonl(protocol::MAGIC);
    std::memcpy(bytes, &magic, 4);
    bytes[4] = protocol::VERSION;
    bytes[5] = static_cast<unsigned char>(type);
    bytes[6] = static_cast<unsigned char>((flags >> 8) & 0xFF);
    bytes[7] = static_cast<unsigned char>(flags & 0xFF);

    const std::uint32_t networkNodeId = htonl(nodeId);
    std::memcpy(bytes + 8, &networkNodeId, 4);

    const std::uint32_t networkSessionId = htonl(sessionId);
    std::memcpy(bytes + 12, &networkSessionId, 4);

    if (type == SessionType::Open) {
        const std::uint16_t networkPort = htons(listenPort);
        std::memcpy(bytes + 16, &networkPort, 2);

        const std::uint32_t networkNonce = htonl(nonce);
        std::memcpy(bytes + 20, &networkNonce, 4);
    } else if (type == SessionType::Accept) {
        const std::uint32_t networkNonce = htonl(nonce);
        std::memcpy(bytes + 16, &networkNonce, 4);
    } else if (type == SessionType::Reject || type == SessionType::Close) {
        bytes[16] = reason;
    }

    return buffer;
}

bool SessionMessage::decode(const void* data, std::size_t size, SessionMessage& message) {
    if (data == nullptr || size < HEADER_SIZE) {
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

    const std::uint8_t rawType = bytes[5];
    if (!isValidType(rawType)) {
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

    const auto type = static_cast<SessionType>(rawType);

    message.type = type;
    message.flags = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[6]) << 8) | bytes[7]);
    message.sessionId = decodedSessionId;
    message.nodeId = decodedNodeId;
    message.nonce = 0;
    message.listenPort = 0;
    message.reason = 0;

    if (type == SessionType::Open) {
        if (size < 24) {
            return false;
        }

        std::uint16_t networkPort = 0;
        std::memcpy(&networkPort, bytes + 16, 2);

        std::uint32_t networkNonce = 0;
        std::memcpy(&networkNonce, bytes + 20, 4);

        const std::uint16_t decodedPort = ntohs(networkPort);
        if (decodedPort == 0) {
            return false;
        }

        message.listenPort = decodedPort;
        message.nonce = ntohl(networkNonce);
        return true;
    }

    if (type == SessionType::Accept) {
        if (size < 20) {
            return false;
        }

        std::uint32_t networkNonce = 0;
        std::memcpy(&networkNonce, bytes + 16, 4);
        message.nonce = ntohl(networkNonce);
        return true;
    }

    if (type == SessionType::Reject || type == SessionType::Close) {
        if (size < 20) {
            return false;
        }

        message.reason = bytes[16];
        return true;
    }

    return size >= 16;
}

} // namespace netproxy::session
