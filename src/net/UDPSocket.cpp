#include "UDPSocket.h"

#include "Log.h"

#include <cerrno>

namespace netproxy::net {

bool UDPSocket::setBroadcast(bool enabled) {
    if (!isValid()) {
        Log::warning("UDP setBroadcast 失败: 套接字无效");
        return false;
    }

    const int value = enabled ? 1 : 0;
#ifdef _WIN32
    const int result =
        setsockopt(nativeHandle(), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&value), sizeof(value));
#else
    const int result = setsockopt(nativeHandle(), SOL_SOCKET, SO_BROADCAST, &value, sizeof(value));
#endif

    if (result != 0) {
        Log::warning("UDP setBroadcast 失败: ", lastErrorMessage());
        return false;
    }
    return true;
}

bool UDPSocket::updateMulticastMembership(const std::string& group, const std::string& interfaceAddress, int option) {
    if (!isValid()) {
        Log::warning("UDP 组播操作失败: 套接字无效");
        return false;
    }

    const IPAddress groupAddress = IPAddress::parse(group, 0);
    if (!groupAddress.isValid() || groupAddress.family() != AF_INET) {
        Log::error("UDP 组播操作失败: 组播地址无效 ", group);
        return false;
    }

    ip_mreq request{};
    request.imr_multiaddr = reinterpret_cast<const sockaddr_in*>(groupAddress.data())->sin_addr;
    request.imr_interface.s_addr = htonl(INADDR_ANY);

    if (!interfaceAddress.empty()) {
        const IPAddress interface = IPAddress::parse(interfaceAddress, 0);
        if (!interface.isValid() || interface.family() != AF_INET) {
            Log::error("UDP 组播操作失败: 接口地址无效 ", interfaceAddress);
            return false;
        }
        request.imr_interface = reinterpret_cast<const sockaddr_in*>(interface.data())->sin_addr;
    }

#ifdef _WIN32
    const int result =
        setsockopt(nativeHandle(), IPPROTO_IP, option, reinterpret_cast<const char*>(&request), sizeof(request));
#else
    const int result = setsockopt(nativeHandle(), IPPROTO_IP, option, &request, sizeof(request));
#endif

    if (result != 0) {
        const std::string error = lastErrorMessage();
        Log::error("UDP 组播操作失败 ", group, ": ", error);
        return false;
    }
    return true;
}

bool UDPSocket::joinMulticastGroup(const std::string& group, const std::string& interfaceAddress) {
    return updateMulticastMembership(group, interfaceAddress, IP_ADD_MEMBERSHIP);
}

bool UDPSocket::leaveMulticastGroup(const std::string& group, const std::string& interfaceAddress) {
    return updateMulticastMembership(group, interfaceAddress, IP_DROP_MEMBERSHIP);
}

bool UDPSocket::setMulticastLoop(bool enabled) {
    if (!isValid()) {
        Log::warning("UDP setMulticastLoop 失败: 套接字无效");
        return false;
    }

#ifdef _WIN32
    const DWORD value = enabled ? 1 : 0;
    const int result =
        setsockopt(nativeHandle(), IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&value), sizeof(value));
#else
    const unsigned char value = enabled ? 1 : 0;
    const int result = setsockopt(nativeHandle(), IPPROTO_IP, IP_MULTICAST_LOOP, &value, sizeof(value));
#endif

    if (result != 0) {
        Log::warning("UDP setMulticastLoop 失败: ", lastErrorMessage());
        return false;
    }
    return true;
}

bool UDPSocket::setMulticastTtl(int ttl) {
    if (!isValid()) {
        Log::warning("UDP setMulticastTtl 失败: 套接字无效");
        return false;
    }

    if (ttl < 0 || ttl > 255) {
        Log::warning("UDP setMulticastTtl 失败: TTL 应在 0-255 之间");
        return false;
    }

#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(ttl);
    const int result =
        setsockopt(nativeHandle(), IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&value), sizeof(value));
#else
    const unsigned char value = static_cast<unsigned char>(ttl);
    const int result = setsockopt(nativeHandle(), IPPROTO_IP, IP_MULTICAST_TTL, &value, sizeof(value));
#endif

    if (result != 0) {
        Log::warning("UDP setMulticastTtl 失败: ", lastErrorMessage());
        return false;
    }
    return true;
}

int UDPSocket::sendTo(const void* data, std::size_t size, const IPAddress& address) {
    if (!address.isValid()) {
        Log::error("UDP sendTo 失败: 目标地址无效");
        return -1;
    }

    if (!isValid() && !open(address)) {
        return -1;
    }

    if (addressFamily() != address.family()) {
        Log::warning("UDP sendTo 失败: 套接字与目标地址族不一致 ", address.toString());
        return -1;
    }

#ifdef _WIN32
    const int sent = ::sendto(nativeHandle(), static_cast<const char*>(data), static_cast<int>(size), 0, address.data(),
                              static_cast<int>(address.length()));
#else
    const int sent = static_cast<int>(::sendto(nativeHandle(), data, size, 0, address.data(), address.length()));
#endif

    if (sent < 0) {
        const std::string error = lastErrorMessage();
        Log::warning("UDP sendTo 失败 ", address.toString(), ": ", error);
    }
    return sent;
}

int UDPSocket::sendTo(const std::string& data, const IPAddress& address) {
    return sendTo(data.data(), data.size(), address);
}

int UDPSocket::receiveFrom(void* buffer, std::size_t size, IPAddress& from) {
    if (!isValid()) {
        Log::warning("UDP receiveFrom 失败: 套接字无效");
        return -1;
    }

    sockaddr_storage sender{};
    SocketLength length = sizeof(sender);

#ifdef _WIN32
    const int received = ::recvfrom(nativeHandle(), static_cast<char*>(buffer), static_cast<int>(size), 0,
                                    reinterpret_cast<sockaddr*>(&sender), &length);
#else
    const int received =
        static_cast<int>(::recvfrom(nativeHandle(), buffer, size, 0, reinterpret_cast<sockaddr*>(&sender), &length));
#endif

    if (received < 0) {
#ifdef _WIN32
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
            return -1;
        }
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
#endif
        Log::warning("UDP receiveFrom 失败: ", lastErrorMessage());
        return -1;
    }

    from = IPAddress::fromSockaddr(reinterpret_cast<const sockaddr*>(&sender), length);
    return received;
}

} // namespace netproxy::net
