#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace netproxy::session {

// 会话报文类型。
enum class SessionType : std::uint8_t { Open = 1, Accept = 2, Reject = 3, Close = 4, Ping = 5 };

// Reject 的原因。
enum class RejectReason : std::uint8_t {
    TooManySessions = 1, // 会话总数超限
    TooManyPerPeer = 2,  // 单对节点会话数超限
    VersionMismatch = 3, // 协议版本不匹配
    UnknownPeer = 4,     // 未知节点
    PolicyRejected = 5   // 被策略拒绝
};

// Close 的原因。
enum class CloseReason : std::uint8_t {
    Local = 1,   // 本地主动关闭
    Remote = 2,  // 收到对端关闭
    Timeout = 3, // 超时
    Error = 4    // 内部错误
};

// Ping 报文的 flags 位：置位表示响应，否则为请求。
constexpr std::uint16_t SESSION_FLAG_RESPONSE = 0x0001;

/**
 * 会话信令报文。
 *
 * 统一头 16 字节：magic(4) + version(1) + type(1) + flags(2) + nodeId(4) + sessionId(4)，
 * 之后按类型追加字段，全部网络字节序：
 * - Open   24 字节：listenPort(2) + reserved(2) + nonce(4)
 * - Accept 20 字节：nonce(4)
 * - Reject 20 字节：reason(1) + reserved(3)
 * - Close  20 字节：reason(1) + reserved(3)
 * - Ping   16 字节：无负载
 */
class SessionMessage final {
public:
    static constexpr std::size_t HEADER_SIZE = 16;
    static constexpr std::size_t MAX_SIZE = 24;

    SessionType type = SessionType::Open;
    std::uint32_t sessionId = 0;
    std::uint16_t flags = 0;      // Ping 用 SESSION_FLAG_RESPONSE 区分请求与响应
    std::uint32_t nodeId = 0;     // 发送方节点标识，每条报文都带
    std::uint32_t nonce = 0;      // 发起方验证码，Accept 回显
    std::uint16_t listenPort = 0; // 发起方数据端口，Open 使用
    std::uint8_t reason = 0;      // Reject / Close 使用

    /**
     * 按类型编码为报文。
     *
     * @return 网络字节序的二进制内容，长度由类型决定。
     */
    std::string encode() const;

    /**
     * 解析报文。
     *
     * 依次校验长度、magic、版本、类型、sessionId 以及类型专属字段。
     *
     * @param data 报文首地址。
     * @param size 报文长度。
     * @param message 解析结果，仅在返回 true 时被写入。
     * @return 解析成功返回 true；任一字段非法返回 false。
     */
    static bool decode(const void* data, std::size_t size, SessionMessage& message);

private:
    // 当前类型对应的报文长度。
    std::size_t encodedSize() const;
};

} // namespace netproxy::session
