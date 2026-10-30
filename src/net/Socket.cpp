#include "Socket.h"

#include "Log.h"

#include <cerrno>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#endif

namespace netproxy::net {

bool Socket::ensureLibrary() {
#ifdef _WIN32
    static const bool initialized = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
#else
    return true;
#endif
}

namespace {

// 从 sockaddr 中取出端口，兼容 IPv4 与 IPv6。
std::uint16_t addressPort(const sockaddr* address) {
    if (address == nullptr) {
        return 0;
    }

    if (address->sa_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(address)->sin_port);
    }
    if (address->sa_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(address)->sin6_port);
    }
    return 0;
}

// 将 sockaddr 转换为数字形式的主机地址。
std::string addressToText(const sockaddr* address, SocketLength length) {
    if (address == nullptr) {
        return std::string();
    }

    char host[NI_MAXHOST] = {};
    if (getnameinfo(address, length, host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST) != 0) {
        return std::string();
    }
    return host;
}

} // namespace

IPAddress IPAddress::parse(const std::string& host, std::uint16_t port) {
    if (!Socket::ensureLibrary()) {
        Log::error("地址解析失败: 套接字库初始化失败");
        return IPAddress();
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* head = nullptr;
    const std::string service = std::to_string(port);
    const int result = getaddrinfo(host.c_str(), service.c_str(), &hints, &head);
    if (result != 0) {
        Log::warning("地址解析失败 ", host, ":", port, " (", result, ")");
        return IPAddress();
    }

    const addrinfo* chosen = nullptr;
    for (const addrinfo* entry = head; entry != nullptr; entry = entry->ai_next) {
        if (entry->ai_family == AF_INET) {
            chosen = entry;
            break;
        }
        if (chosen == nullptr && entry->ai_family == AF_INET6) {
            chosen = entry;
        }
    }

    if (chosen == nullptr) {
        Log::warning("地址解析失败 ", host, ":", port, ": 无可用地址");
    }

    IPAddress address;
    if (chosen != nullptr) {
        address.length_ = static_cast<SocketLength>(chosen->ai_addrlen);
        std::memcpy(&address.storage_, chosen->ai_addr, chosen->ai_addrlen);
    }

    freeaddrinfo(head);
    return address;
}

IPAddress IPAddress::loopback(std::uint16_t port) {
    return parse("127.0.0.1", port);
}

IPAddress IPAddress::any(std::uint16_t port) {
    return parse("0.0.0.0", port);
}

IPAddress IPAddress::fromSockaddr(const sockaddr* address, SocketLength length) {
    IPAddress result;
    if (address == nullptr || length == 0) {
        return result;
    }

    std::memcpy(&result.storage_, address, static_cast<std::size_t>(length));
    result.length_ = length;
    return result;
}

bool IPAddress::isValid() const {
    return length_ != 0;
}

int IPAddress::family() const {
    if (!isValid()) {
        return AF_UNSPEC;
    }
    return storage_.ss_family;
}

std::uint16_t IPAddress::port() const {
    if (!isValid()) {
        return 0;
    }
    return addressPort(reinterpret_cast<const sockaddr*>(&storage_));
}

std::string IPAddress::host() const {
    if (!isValid()) {
        return std::string();
    }
    return addressToText(reinterpret_cast<const sockaddr*>(&storage_), length_);
}

std::string IPAddress::toString() const {
    if (!isValid()) {
        return std::string();
    }

    if (family() == AF_INET6) {
        return "[" + host() + "]:" + std::to_string(port());
    }
    return host() + ":" + std::to_string(port());
}

const sockaddr* IPAddress::data() const {
    return reinterpret_cast<const sockaddr*>(&storage_);
}

SocketLength IPAddress::length() const {
    return length_;
}

Socket::Socket(int socketType, int protocol) : socketType_(socketType), protocol_(protocol) {}

Socket::Socket(SocketHandle handle, int socketType, int protocol)
    : socketType_(socketType), protocol_(protocol), handle_(handle) {}

Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept
    : socketType_(other.socketType_), protocol_(other.protocol_), handle_(other.handle_), family_(other.family_) {
    other.handle_ = INVALID_SOCKET_HANDLE;
    other.family_ = AF_UNSPEC;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        socketType_ = other.socketType_;
        protocol_ = other.protocol_;
        handle_ = other.handle_;
        family_ = other.family_;
        other.handle_ = INVALID_SOCKET_HANDLE;
        other.family_ = AF_UNSPEC;
    }
    return *this;
}

bool Socket::isValid() const {
    return handle_ != INVALID_SOCKET_HANDLE;
}

SocketHandle Socket::nativeHandle() const {
    return handle_;
}

void Socket::close() {
    if (!isValid()) {
        return;
    }

#ifdef _WIN32
    ::closesocket(handle_);
#else
    ::close(handle_);
#endif

    handle_ = INVALID_SOCKET_HANDLE;
    family_ = AF_UNSPEC;
}

int Socket::lastErrorCode() const {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string Socket::lastErrorMessage() const {
#ifdef _WIN32
    const int errorCode = WSAGetLastError();
    char* buffer = nullptr;
    const DWORD length =
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, static_cast<DWORD>(errorCode), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       reinterpret_cast<LPSTR>(&buffer), 0, nullptr);

    if (length == 0 || buffer == nullptr) {
        return "socket error " + std::to_string(errorCode);
    }

    std::string message(buffer, length);
    LocalFree(buffer);

    while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
        message.pop_back();
    }
    return message;
#else
    return std::strerror(errno);
#endif
}

std::uint16_t Socket::localPort() const {
    if (!isValid()) {
        return 0;
    }

    sockaddr_storage address{};
    SocketLength length = sizeof(address);
    if (getsockname(handle_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        Log::warning("获取本地端口失败: ", lastErrorMessage());
        return 0;
    }
    return addressPort(reinterpret_cast<const sockaddr*>(&address));
}

int Socket::addressFamily() const {
    if (!isValid()) {
        return AF_UNSPEC;
    }

    // 创建时已记录地址族，直接使用。Windows 上对未绑定的套接字调用 getsockname 会返回
    // WSAEINVAL，用它反查会把 AF_INET 误判成 AF_UNSPEC，导致 bind 前检查失败。
    if (family_ != AF_UNSPEC) {
        return family_;
    }

    // 接管的外部句柄没有创建时的地址族，退回系统查询。
    sockaddr_storage address{};
    SocketLength length = sizeof(address);
    if (getsockname(handle_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return AF_UNSPEC;
    }
    return address.ss_family;
}

bool Socket::setReuseAddress(bool enabled) {
    if (!isValid()) {
        Log::warning("设置地址复用失败: 套接字无效");
        return false;
    }

    const int value = enabled ? 1 : 0;
#ifdef _WIN32
    const int result =
        setsockopt(handle_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&value), sizeof(value));
#else
    const int result = setsockopt(handle_, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
#endif

    if (result != 0) {
        Log::warning("设置地址复用失败: ", lastErrorMessage());
        return false;
    }
    return true;
}

bool Socket::setNonBlocking(bool enabled) {
    if (!isValid()) {
        Log::warning("设置非阻塞失败: 套接字无效");
        return false;
    }

#ifdef _WIN32
    u_long mode = enabled ? 1 : 0;
    if (::ioctlsocket(handle_, FIONBIO, &mode) != 0) {
        Log::warning("设置非阻塞失败: ", lastErrorMessage());
        return false;
    }
    return true;
#else
    const int flags = ::fcntl(handle_, F_GETFL, 0);
    if (flags < 0) {
        Log::warning("设置非阻塞失败: ", lastErrorMessage());
        return false;
    }

    const int updated = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(handle_, F_SETFL, updated) != 0) {
        Log::warning("设置非阻塞失败: ", lastErrorMessage());
        return false;
    }
    return true;
#endif
}

bool Socket::open(const IPAddress& address) {
    if (!address.isValid()) {
        Log::error("创建套接字失败: 地址无效");
        return false;
    }
    return create(address.family());
}

bool Socket::bind(const IPAddress& address) {
    if (!address.isValid()) {
        Log::error("绑定失败: 地址无效");
        return false;
    }

    if (!isValid() && !create(address.family())) {
        return false;
    }

    if (addressFamily() != address.family()) {
        Log::error("绑定失败: 套接字与地址族不一致 ", address.toString());
        return false;
    }

    if (::bind(nativeHandle(), address.data(), address.length()) != 0) {
        const std::string error = lastErrorMessage();
        Log::error("绑定失败 ", address.toString(), ": ", error);
        return false;
    }

    Log::debug("绑定成功 ", address.toString());
    return true;
}

bool Socket::create(int family) {
    // 已持有句柄时不再重建：静默关闭可能销毁外部仍在使用的句柄。
    if (isValid()) {
        Log::debug("创建套接字跳过: 已持有句柄");
        return false;
    }

    if (!ensureLibrary()) {
        Log::error("创建套接字失败: 套接字库初始化失败");
        return false;
    }

    const SocketHandle handle = ::socket(family, socketType_, protocol_);
    if (handle == INVALID_SOCKET_HANDLE) {
        Log::error("创建套接字失败: ", lastErrorMessage());
        return false;
    }

    handle_ = handle;
    family_ = family;
    return true;
}

} // namespace netproxy::net
