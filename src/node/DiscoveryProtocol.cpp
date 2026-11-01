#include "DiscoveryProtocol.h"

#include <cstring>

namespace netproxy::node {

namespace {

constexpr std::uint32_t PROTOCOL_MAGIC = 0x4E50524F; // "NPRO"
constexpr std::uint8_t PROTOCOL_VERSION = 1;

// 从 IPv4 地址取出网络序的四字节表示，无效地址返回 0。
std::uint32_t addressValue(const net::IPAddress& address) {
    if (!address.isValid() || address.family() != AF_INET) {
        return 0;
    }
    return reinterpret_cast<const sockaddr_in*>(address.data())->sin_addr.s_addr;
}

// 用网络序的四字节地址构造 IPAddress，0 返回无效地址。
net::IPAddress toAddress(std::uint32_t value) {
    if (value == 0) {
        return net::IPAddress();
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = value;
    return net::IPAddress::fromSockaddr(reinterpret_cast<const sockaddr*>(&address),
                                        static_cast<net::SocketLength>(sizeof(address)));
}

} // namespace

std::string AnnounceMessage::encode() const {
    std::string buffer(SIZE, '\0');
    auto* bytes = reinterpret_cast<unsigned char*>(&buffer[0]);

    const std::uint32_t magic = htonl(PROTOCOL_MAGIC);
    std::memcpy(bytes, &magic, 4);
    bytes[4] = PROTOCOL_VERSION;
    bytes[5] = static_cast<unsigned char>(type);

    const std::uint32_t networkAddress = addressValue(address);
    std::memcpy(bytes + 6, &networkAddress, 4);

    const std::uint16_t networkPort = htons(port);
    std::memcpy(bytes + 10, &networkPort, 2);

    const std::uint32_t networkNodeId = htonl(nodeId);
    std::memcpy(bytes + 12, &networkNodeId, 4);

    return buffer;
}

bool AnnounceMessage::decode(const void* data, std::size_t size, AnnounceMessage& message) {
    if (data == nullptr || size < SIZE) {
        return false;
    }

    const auto* bytes = static_cast<const unsigned char*>(data);

    std::uint32_t magic = 0;
    std::memcpy(&magic, bytes, 4);
    if (ntohl(magic) != PROTOCOL_MAGIC) {
        return false;
    }

    if (bytes[4] != PROTOCOL_VERSION) {
        return false;
    }

    const std::uint8_t rawType = bytes[5];
    if (rawType != static_cast<std::uint8_t>(AnnounceType::Announce) &&
        rawType != static_cast<std::uint8_t>(AnnounceType::Bye)) {
        return false;
    }

    std::uint32_t networkAddress = 0;
    std::memcpy(&networkAddress, bytes + 6, 4);

    std::uint16_t networkPort = 0;
    std::memcpy(&networkPort, bytes + 10, 2);
    const std::uint16_t decodedPort = ntohs(networkPort);
    if (decodedPort == 0) {
        return false;
    }

    std::uint32_t networkNodeId = 0;
    std::memcpy(&networkNodeId, bytes + 12, 4);

    message.type = static_cast<AnnounceType>(rawType);
    message.address = toAddress(networkAddress);
    message.port = decodedPort;
    message.nodeId = ntohl(networkNodeId);
    return true;
}

} // namespace netproxy::node
