#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "protocol/Protocol.h"

namespace netproxy::transport {

/**
 * 握手报文。
 *
 * 版本 2，走裸 UDP，可重发；只负责交换 nodeId 与 nonce。
 */
struct HelloMessage {
    std::uint8_t type = static_cast<std::uint8_t>(protocol::HelloType::Hello);
    std::uint32_t nodeId = 0;
    std::uint32_t nonce = 0;
    std::uint8_t reason = 0; // HelloReject 使用
};

/**
 * 连接帧。
 *
 * 所有帧共用一个连接级序号空间，`streamId` 为 0 表示连接控制。
 */
struct PeerFrame {
    std::uint8_t type = static_cast<std::uint8_t>(protocol::FrameType::Data);
    std::uint32_t nodeId = 0;
    std::uint32_t connectionSeq = 0;
    std::uint32_t acknowledge = 0;
    std::uint16_t window = 0;
    std::uint16_t payloadLength = 0;
    std::uint32_t streamId = 0;
    std::string payload;
};

/**
 * 连接协议编解码，纯函数，不保存状态。
 */
class PeerProtocol final {
public:
    static constexpr std::size_t HELLO_HEADER_SIZE = protocol::HELLO_HEADER_SIZE;
    static constexpr std::size_t FRAME_HEADER_SIZE = protocol::FRAME_HEADER_SIZE;

    /**
     * 编码握手报文。
     *
     * @param message 握手内容。
     * @return 16 字节网络序报文。
     */
    static std::string encodeHello(const HelloMessage& message);

    /**
     * 解析握手报文。
     *
     * @param data 报文首地址。
     * @param size 报文长度。
     * @param message 解析结果，仅在返回 true 时写入。
     * @return 解析成功返回 true。
     */
    static bool decodeHello(const void* data, std::size_t size, HelloMessage& message);

    /**
     * 编码连接帧。
     *
     * @param frame 帧内容。
     * @return 28 字节头 + 负载。
     */
    static std::string encodeFrame(const PeerFrame& frame);

    /**
     * 解析连接帧。
     *
     * @param data 报文首地址。
     * @param size 报文长度。
     * @param frame 解析结果，仅在返回 true 时写入。
     * @return 解析成功返回 true。
     */
    static bool decodeFrame(const void* data, std::size_t size, PeerFrame& frame);
};

} // namespace netproxy::transport
