#include "TCPSocket.h"

#include "Log.h"

#ifndef _WIN32
#include <netinet/tcp.h>
#endif

namespace netproxy::net {

#ifndef _WIN32
namespace {

// 发送标志：POSIX 下避免对端关闭时触发 SIGPIPE。
int dataSendFlags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

} // namespace
#endif

bool TCPSocket::connect(const IPAddress& address) {
    if (!address.isValid()) {
        Log::error("TCP connect failed: invalid target address");
        return false;
    }

    if (!open(address)) {
        return false;
    }

    if (::connect(nativeHandle(), address.data(), address.length()) != 0) {
        const std::string error = lastErrorMessage();
        Log::warning("TCP connect failed ", address.toString(), ": ", error);
        return false;
    }

    Log::debug("TCP connection established ", address.toString());
    return true;
}

bool TCPSocket::listen(int backlog) {
    if (!isValid()) {
        Log::error("TCP listen failed: invalid socket");
        return false;
    }

    if (::listen(nativeHandle(), backlog) != 0) {
        Log::error("TCP listen failed: ", lastErrorMessage());
        return false;
    }
    return true;
}

TCPSocket TCPSocket::accept() {
    if (!isValid()) {
        Log::error("TCP accept failed: invalid socket");
        return TCPSocket(INVALID_SOCKET_HANDLE);
    }

    const SocketHandle handle = ::accept(nativeHandle(), nullptr, nullptr);
    if (handle == INVALID_SOCKET_HANDLE) {
        Log::warning("TCP accept failed: ", lastErrorMessage());
    } else {
        Log::debug("TCP connection accepted handle=", handle);
    }
    return TCPSocket(handle);
}

int TCPSocket::send(const void* data, std::size_t size) {
    if (!isValid()) {
        Log::warning("TCP send failed: invalid socket");
        return -1;
    }

#ifdef _WIN32
    const int sent = ::send(nativeHandle(), static_cast<const char*>(data), static_cast<int>(size), 0);
#else
    const int sent = static_cast<int>(::send(nativeHandle(), data, size, dataSendFlags()));
#endif

    if (sent < 0) {
        Log::warning("TCP send failed: ", lastErrorMessage());
    }
    return sent;
}

int TCPSocket::send(const std::string& data) {
    return send(data.data(), data.size());
}

int TCPSocket::receive(void* buffer, std::size_t size) {
    if (!isValid()) {
        Log::warning("TCP receive failed: invalid socket");
        return -1;
    }

#ifdef _WIN32
    const int received = ::recv(nativeHandle(), static_cast<char*>(buffer), static_cast<int>(size), 0);
#else
    const int received = static_cast<int>(::recv(nativeHandle(), buffer, size, 0));
#endif

    if (received < 0) {
        Log::warning("TCP receive failed: ", lastErrorMessage());
    }
    return received;
}

std::string TCPSocket::receive(std::size_t maxSize) {
    if (maxSize == 0) {
        return std::string();
    }

    std::string buffer(maxSize, '\0');
    const int received = receive(&buffer[0], maxSize);
    if (received <= 0) {
        return std::string();
    }

    buffer.resize(static_cast<std::size_t>(received));
    return buffer;
}

bool TCPSocket::shutdownWrite() {
    if (!isValid()) {
        Log::warning("TCP shutdownWrite failed: invalid socket");
        return false;
    }

#ifdef _WIN32
    const int result = ::shutdown(nativeHandle(), SD_SEND);
#else
    const int result = ::shutdown(nativeHandle(), SHUT_WR);
#endif

    if (result != 0) {
        Log::warning("TCP shutdownWrite failed: ", lastErrorMessage());
        return false;
    }
    return true;
}

bool TCPSocket::setNoDelay(bool enabled) {
    if (!isValid()) {
        Log::warning("TCP setNoDelay failed: invalid socket");
        return false;
    }

    const int value = enabled ? 1 : 0;
#ifdef _WIN32
    const int result =
        setsockopt(nativeHandle(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&value), sizeof(value));
#else
    const int result = setsockopt(nativeHandle(), IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
#endif

    if (result != 0) {
        Log::warning("TCP setNoDelay failed: ", lastErrorMessage());
        return false;
    }
    return true;
}

} // namespace netproxy::net
