#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace netproxy::transport {

/**
 * 数据传输报文。
 *
 * 复用 16 字节通用头，按 type 区分：
 * - Data（6）：头部 28 字节，携带序号、确认、窗口与负载
 * - Ack（7）：头部 24 字节，只携带确认与窗口
 */
class DataMessage final {
public:
    static constexpr std::size_t DATA_HEADER_SIZE = 28;
    static constexpr std::size_t ACK_HEADER_SIZE = 24;

    std::uint8_t type = 6; // 6=Data，7=Ack
    std::uint32_t sessionId = 0;
    std::uint32_t nodeId = 0;
    std::uint32_t sequence = 0;      // Data 使用
    std::uint32_t acknowledge = 0;   // 累计确认
    std::uint16_t window = 0;        // 接收方剩余缓存（报文数）
    std::uint16_t payloadLength = 0; // Data 使用
    std::string payload;             // Data 使用

    /**
     * 编码为完整报文，包含通用头。
     *
     * @return 网络字节序的二进制内容；Data 为 28 + 负载，Ack 为 24。
     */
    std::string encodePacket() const;

    /**
     * 解析完整报文。
     *
     * 依次校验长度、magic、版本、类型、nodeId、sessionId 与负载长度。
     *
     * @param data 报文首地址。
     * @param size 报文长度。
     * @param message 解析结果，仅在返回 true 时写入。
     * @return 解析成功返回 true；任一字段非法返回 false。
     */
    static bool decodePacket(const void* data, std::size_t size, DataMessage& message);
};

} // namespace netproxy::transport
