#include "WakeupChannel.h"

#include "Log.h"

#include <cstring>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace netproxy::net {

#ifdef _WIN32
namespace {

// 创建绑定到回环地址的非阻塞 UDP 套接字。
SocketHandle createWakeupSocket() {
    const SocketHandle handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == INVALID_SOCKET_HANDLE) {
        return INVALID_SOCKET_HANDLE;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(handle, reinterpret_cast<const sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
        ::closesocket(handle);
        return INVALID_SOCKET_HANDLE;
    }

    u_long mode = 1;
    if (::ioctlsocket(handle, FIONBIO, &mode) != 0) {
        ::closesocket(handle);
        return INVALID_SOCKET_HANDLE;
    }
    return handle;
}

// 让 from 连接到 to 的地址，之后可直接使用 send。
bool connectToPeer(SocketHandle from, SocketHandle to) {
    sockaddr_in address{};
    int length = static_cast<int>(sizeof(address));
    if (::getsockname(to, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return false;
    }
    return ::connect(from, reinterpret_cast<const sockaddr*>(&address), static_cast<int>(sizeof(address))) == 0;
}

} // namespace
#endif

WakeupChannel::~WakeupChannel() {
    close();
}

bool WakeupChannel::open() {
    close();

#ifdef _WIN32
    // 唤醒通道直接调用 Winsock，需先确保套接字库已初始化，否则 socket 返回
    // WSANOTINITIALISED（10093）。
    if (!Socket::ensureLibrary()) {
        Log::error("failed to create wakeup channel: socket library init failed");
        return false;
    }

    const SocketHandle reader = createWakeupSocket();
    const SocketHandle writer = createWakeupSocket();
    if (reader == INVALID_SOCKET_HANDLE || writer == INVALID_SOCKET_HANDLE) {
        if (reader != INVALID_SOCKET_HANDLE) {
            ::closesocket(reader);
        }
        if (writer != INVALID_SOCKET_HANDLE) {
            ::closesocket(writer);
        }
        Log::error("failed to create wakeup channel: ", WSAGetLastError());
        return false;
    }

    if (!connectToPeer(writer, reader)) {
        ::closesocket(reader);
        ::closesocket(writer);
        Log::error("failed to create wakeup channel: ", WSAGetLastError());
        return false;
    }

    readHandle_ = reader;
    writeHandle_ = writer;
    return true;
#else
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        Log::error("failed to create wakeup pipe: ", std::strerror(errno));
        return false;
    }

    for (int index = 0; index < 2; ++index) {
        const int flags = ::fcntl(fds[index], F_GETFL, 0);
        if (flags < 0 || ::fcntl(fds[index], F_SETFL, flags | O_NONBLOCK) != 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            Log::error("failed to create wakeup pipe: ", std::strerror(errno));
            return false;
        }
    }

    readHandle_ = fds[0];
    writeHandle_ = fds[1];
    return true;
#endif
}

void WakeupChannel::close() {
#ifdef _WIN32
    if (readHandle_ != INVALID_SOCKET_HANDLE) {
        ::closesocket(readHandle_);
        readHandle_ = INVALID_SOCKET_HANDLE;
    }
    if (writeHandle_ != INVALID_SOCKET_HANDLE) {
        ::closesocket(writeHandle_);
        writeHandle_ = INVALID_SOCKET_HANDLE;
    }
#else
    if (readHandle_ != INVALID_SOCKET_HANDLE) {
        ::close(readHandle_);
        readHandle_ = INVALID_SOCKET_HANDLE;
    }
    if (writeHandle_ != INVALID_SOCKET_HANDLE) {
        ::close(writeHandle_);
        writeHandle_ = INVALID_SOCKET_HANDLE;
    }
#endif
}

bool WakeupChannel::isOpen() const {
    return readHandle_ != INVALID_SOCKET_HANDLE && writeHandle_ != INVALID_SOCKET_HANDLE;
}

SocketHandle WakeupChannel::handle() const {
    return readHandle_;
}

void WakeupChannel::notify() {
    if (writeHandle_ == INVALID_SOCKET_HANDLE) {
        return;
    }

    const char byte = 1;
#ifdef _WIN32
    ::send(writeHandle_, &byte, 1, 0);
#else
    const ssize_t ignored = ::write(writeHandle_, &byte, 1);
    static_cast<void>(ignored);
#endif
}

void WakeupChannel::drain() {
    if (readHandle_ == INVALID_SOCKET_HANDLE) {
        return;
    }

    char buffer[64];
#ifdef _WIN32
    while (::recvfrom(readHandle_, buffer, static_cast<int>(sizeof(buffer)), 0, nullptr, nullptr) > 0) {
    }
#else
    while (::read(readHandle_, buffer, sizeof(buffer)) > 0) {
    }
#endif
}

} // namespace netproxy::net
