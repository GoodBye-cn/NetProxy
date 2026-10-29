#pragma once

#include <cstddef>
#include <cstdint>

namespace netproxy::protocol {

// 会话与数据报文共用的通用头常量。
constexpr std::uint32_t MAGIC = 0x4E50534E; // ASCII "NPSN"
constexpr std::uint8_t VERSION = 1;
constexpr std::size_t HEADER_SIZE = 16;

// 报文类型。
enum class MessageType : std::uint8_t { Open = 1, Accept = 2, Reject = 3, Close = 4, Ping = 5, Data = 6, Ack = 7 };

// 关闭与断链原因，信令与数据传输共用。
enum class CloseReason : std::uint8_t {
    Local = 1,   // 本地主动关闭
    Remote = 2,  // 收到对端关闭
    Timeout = 3, // 超时
    Error = 4    // 内部错误或重传耗尽
};

// 判断是否为数据传输报文。
constexpr bool isDataMessage(std::uint8_t type) {
    return type == static_cast<std::uint8_t>(MessageType::Data) || type == static_cast<std::uint8_t>(MessageType::Ack);
}

} // namespace netproxy::protocol
