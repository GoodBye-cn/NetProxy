#pragma once

#include <cstddef>
#include <cstdint>

namespace netproxy::protocol {

// 连接协议常量。
constexpr std::uint32_t MAGIC = 0x4E50534E; // ASCII "NPSN"
constexpr std::uint8_t PEER_VERSION = 2;
constexpr std::size_t HELLO_HEADER_SIZE = 16;
constexpr std::size_t FRAME_HEADER_SIZE = 28;

// 握手报文类型。
enum class HelloType : std::uint8_t { Hello = 1, HelloAck = 2, HelloReject = 3 };

// 握手拒绝原因。
enum class HelloRejectReason : std::uint8_t {
    TooManyConnections = 1,
    UnknownNode = 2,
    VersionMismatch = 3,
    PolicyRejected = 4
};

// 连接帧类型。
enum class FrameType : std::uint8_t {
    Data = 10,
    StreamOpen = 11,
    StreamAccept = 12,
    StreamReject = 13,
    StreamClose = 14,
    Ping = 15,
    Close = 16
};

// 流事件。
enum class StreamEvent : std::uint8_t { Opened = 1, Rejected = 2, Closed = 3 };

// 流与连接关闭原因。
enum class CloseReason : std::uint8_t { Local = 1, Remote = 2, Timeout = 3, Error = 4 };

} // namespace netproxy::protocol
