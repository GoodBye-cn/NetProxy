#pragma once

#include <cstdint>
#include <string>

// 平台差异：Windows 使用 Winsock，其它平台使用 POSIX socket。
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace netproxy::net {

#ifdef _WIN32
using SocketHandle = SOCKET;
using SocketLength = int;
constexpr SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
using SocketHandle = int;
using SocketLength = socklen_t;
constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
#endif

/**
 * 屏蔽 sockaddr 系列平台差异的地址值类型。
 *
 * 内部持有 sockaddr_storage，同时支持 IPv4 与 IPv6，
 * 可直接转换为 bind / connect / sendto 需要的底层结构。
 */
class IPAddress final {
public:
    IPAddress() = default;

    /**
     * 解析 IPv4 / IPv6 字面量或域名。
     *
     * 通过 getaddrinfo 解析，结果优先选择 IPv4，没有 IPv4 记录时回退到 IPv6。
     *
     * @param host 主机名或 IP 字面量，如 example.com、127.0.0.1、::1。
     * @param port 端口号，取 0 时表示由系统分配。
     * @return 解析成功返回有效地址；解析失败返回无效地址，此时 isValid() 为 false。
     */
    static IPAddress parse(const std::string& host, std::uint16_t port);

    /**
     * 构造回环地址。
     *
     * @param port 端口号。
     * @return 127.0.0.1:port，解析失败时为无效地址。
     */
    static IPAddress loopback(std::uint16_t port);

    /**
     * 构造通配地址，用于监听全部网卡。
     *
     * @param port 端口号。
     * @return 0.0.0.0:port，解析失败时为无效地址。
     */
    static IPAddress any(std::uint16_t port);

    /**
     * 从已有的 sockaddr 构造地址。
     *
     * 常用于 recvfrom / accept 之后取出对端地址。
     *
     * @param address 底层地址结构指针，可为空。
     * @param length 地址结构长度，为 0 时返回无效地址。
     * @return 有效地址或无效地址。
     */
    static IPAddress fromSockaddr(const sockaddr* address, SocketLength length);

    /**
     * 判断地址是否有效。
     *
     * @return 已成功构造或解析返回 true。
     */
    bool isValid() const;

    /**
     * 获取地址族。
     *
     * @return AF_INET 或 AF_INET6；地址无效时返回 AF_UNSPEC。
     */
    int family() const;

    /**
     * 获取端口号。
     *
     * @return 主机字节序的端口号；地址无效时返回 0。
     */
    std::uint16_t port() const;

    /**
     * 获取数字形式的主机地址。
     *
     * @return 如 127.0.0.1；地址无效或转换失败时返回空串。
     */
    std::string host() const;

    /**
     * 获取完整的可读地址。
     *
     * @return IPv4 形如 127.0.0.1:80，IPv6 形如 [::1]:80；地址无效时返回空串。
     */
    std::string toString() const;

    /**
     * 获取底层地址结构指针。
     *
     * @return 指向内部 sockaddr_storage 的 sockaddr 指针，生命周期与当前对象一致。
     */
    const sockaddr* data() const;

    /**
     * 获取底层地址结构长度。
     *
     * @return 有效的 sockaddr 长度；地址无效时返回 0。
     */
    SocketLength length() const;

private:
    sockaddr_storage storage_{};
    SocketLength length_ = 0;
};

/**
 * 套接字句柄的 RAII 持有者。
 *
 * 负责句柄的创建与关闭、错误查询以及通用套接字选项。
 * 派生类在构造时传入套接字类型与协议，基类的 open / bind 据此自动创建句柄，
 * 因此 TCPSocket / UDPSocket 不需要各自实现创建与绑定逻辑。
 */
class Socket {
public:
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    /**
     * 移交句柄所有权。
     *
     * @param other 源对象，移交后变为无效。
     */
    Socket(Socket&& other) noexcept;

    /**
     * 移交句柄所有权，先关闭当前持有的句柄。
     *
     * @param other 源对象，移交后变为无效。
     * @return 当前对象引用。
     */
    Socket& operator=(Socket&& other) noexcept;

    // 析构时关闭句柄，重复关闭安全。
    virtual ~Socket();

    /**
     * 判断句柄是否有效。
     *
     * @return 持有有效句柄返回 true。
     */
    bool isValid() const;

    /**
     * 获取原生句柄。
     *
     * @return POSIX 为文件描述符，Windows 为 SOCKET；未创建时返回 INVALID_SOCKET_HANDLE。
     */
    SocketHandle nativeHandle() const;

    // 关闭句柄；未创建时不做任何事，重复调用安全。
    void close();

    /**
     * 获取最近一次 socket 操作的错误码。
     *
     * @return POSIX 为 errno，Windows 为 WSAGetLastError() 的返回值。
     */
    int lastErrorCode() const;

    /**
     * 获取最近一次错误的可读描述。
     *
     * @return 系统错误文本；Windows 下通过 FormatMessage 获取并去掉尾部换行。
     */
    std::string lastErrorMessage() const;

    /**
     * 获取本地绑定的端口。
     *
     * @return 主机字节序端口号；未绑定或查询失败返回 0。
     */
    std::uint16_t localPort() const;

    /**
     * 开启或关闭地址复用（SO_REUSEADDR）。
     *
     * @param enabled true 开启，false 关闭。
     * @return 设置成功返回 true；句柄无效或系统调用失败返回 false。
     * @note 必须在 bind 之前调用才能对后续绑定生效。
     */
    bool setReuseAddress(bool enabled);

    /**
     * 切换阻塞模式。
     *
     * @param enabled true 为非阻塞，false 为阻塞。
     * @return 设置成功返回 true；句柄无效或系统调用失败返回 false。
     * @note 非阻塞模式下接收操作无数据时返回 -1，错误码为 EAGAIN / WSAEWOULDBLOCK。
     */
    bool setNonBlocking(bool enabled);

    /**
     * 创建套接字，但不执行绑定。
     *
     * 使用构造时记录的套接字类型与协议创建句柄，若已持有句柄会先关闭。
     * 适用于需要先设置地址复用等选项再绑定的场景。
     *
     * @param address 决定地址族的地址，IPv4 对应 AF_INET，IPv6 对应 AF_INET6。
     * @return 创建成功返回 true；地址无效或系统调用失败返回 false，并已记录日志。
     */
    bool open(const IPAddress& address);

    /**
     * 将套接字绑定到本地地址。
     *
     * 未创建句柄时先按构造时的类型创建，再执行绑定。
     *
     * @param address 绑定地址，包含 IP 与端口；端口为 0 时由系统分配。
     * @return 成功返回 true；地址无效、地址族不一致或系统调用失败返回 false，并已记录日志。
     * @note 绑定失败会关闭句柄，对象回到未创建状态。
     */
    bool bind(const IPAddress& address);

protected:
    /**
     * 构造套接字对象，尚未创建句柄。
     *
     * @param socketType 套接字类型，如 SOCK_STREAM、SOCK_DGRAM。
     * @param protocol 协议，如 IPPROTO_TCP、IPPROTO_UDP。
     */
    Socket(int socketType, int protocol);

    /**
     * 接管一个已创建的句柄。
     *
     * @param handle 原生句柄，所有权移交当前对象。
     * @param socketType 套接字类型，供后续重新创建时使用。
     * @param protocol 协议，供后续重新创建时使用。
     */
    Socket(SocketHandle handle, int socketType, int protocol);

    /**
     * 查询已创建套接字的地址族。
     *
     * @return AF_INET / AF_INET6；未创建或查询失败返回 AF_UNSPEC。
     */
    int addressFamily() const;

private:
    /**
     * 按平台差异创建指定地址族的套接字。
     *
     * @param family AF_INET 或 AF_INET6。
     * @return 创建成功返回 true；库未初始化或系统调用失败返回 false。
     */
    bool create(int family);

    /**
     * 确保套接字库可用。
     *
     * @return 可继续使用返回 true；Windows 下 WSAStartup 失败返回 false，其它平台恒为 true。
     */
    static bool ensureLibrary();

    friend class IPAddress;

    int socketType_ = 0;
    int protocol_ = 0;
    SocketHandle handle_ = INVALID_SOCKET_HANDLE;
};

} // namespace netproxy::net
