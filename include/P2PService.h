#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "PeerInfo.h"

namespace netproxy {

// 流事件。
enum class StreamEvent {
    Opened,   // 流已打开
    Rejected, // 建连失败、流上限或对端拒绝
    Closed    // 流已关闭
};

// 流快照。
struct StreamInfo {
    std::uint32_t streamId = 0;
    std::uint32_t peerNodeId = 0;
    bool open = false; // 是否已打开
};

// P2P 服务配置。
struct P2PServiceConfig {
    std::uint32_t nodeId = 0;    // 0 表示启动时随机生成
    std::uint16_t dataPort = 0;  // 0 表示由系统分配
    std::string dataBindAddress; // 空串表示 0.0.0.0

    std::string multicastGroup = "239.255.42.99"; // 发现通道组播组
    std::uint16_t multicastPort = 45454;          // 发现通道组播端口
    int announceIntervalMs = 3000;                // 公告周期
    int peerTimeoutMs = 10000;                    // 节点过期时间

    // 连接与流
    std::size_t maxStreams = 64;       // 流总数上限
    std::size_t maxStreamsPerPeer = 8; // 单连接流上限
    int helloRetryIntervalMs = 1000;   // Hello 重发间隔
    int helloRetryCount = 5;           // Hello 最多重发次数

    // 可靠传输
    std::size_t maxPayloadSize = 1400; // 单帧负载上限
    std::size_t sendWindow = 16;       // 发送窗口（帧数）
    std::size_t receiveWindow = 64;    // 接收缓存（帧数）
    int retransmitTimeoutMs = 200;     // 重传超时
    int maxRetransmitCount = 5;        // 单帧重传上限

    // 心跳
    int pingIntervalMs = 30000; // 空闲多久发 Ping
    int pingTimeoutMs = 10000;  // 每次 Ping 等待时间
    int pingRetryCount = 3;     // 连续失败上限
};

/**
 * 局域网 P2P 节点服务。
 *
 * 内部包含一个 I/O 线程与一个业务线程：
 * - I/O 线程负责数据 socket 与组播 socket 的读写；
 * - 业务线程负责节点表、连接状态机、流表、周期公告与过期淘汰。
 *
 * 不设独立的会话层：对端之间一条可靠连接，业务在连接上开流。
 * 通过 pimpl 隐藏内部实现，外部只需包含本头文件。
 */
class P2PService final {
public:
    /**
     * 发现新节点时的回调。
     *
     * @param peer 对端地址、数据端口与节点标识。
     * @note 回调运行在业务线程，不能执行阻塞操作，也不要调用 stop()。
     */
    using PeerHandler = std::function<void(const PeerInfo& peer)>;

    /**
     * 流事件回调。
     *
     * @param streamId 流标识。
     * @param peerNodeId 对端节点标识。
     * @param event 事件类型。
     * @param reason 关闭或拒绝原因，Opened 时为 0。
     * @note 回调运行在业务线程，不能执行阻塞操作，也不要调用 stop()。
     */
    using StreamHandler =
        std::function<void(std::uint32_t streamId, std::uint32_t peerNodeId, StreamEvent event, std::uint8_t reason)>;

    /**
     * 数据交付回调。
     *
     * @param streamId 流标识。
     * @param peerNodeId 对端节点标识。
     * @param data 数据内容，回调返回后失效，需要保留请自行拷贝。
     * @param size 数据长度。
     * @note 回调运行在业务线程，不能执行阻塞操作，也不要调用 stop()。
     */
    using DataHandler =
        std::function<void(std::uint32_t streamId, std::uint32_t peerNodeId, const char* data, std::size_t size)>;

    // 构造服务，不启动线程。
    P2PService();

    // 析构时自动停止。
    ~P2PService();

    P2PService(const P2PService&) = delete;
    P2PService& operator=(const P2PService&) = delete;

    /**
     * 启动服务。
     *
     * 顺序：绑定数据 socket -> 打开组播服务 -> 启动节点管理 -> 启动连接管理 ->
     * 启动 I/O 线程 -> 启动业务线程。
     *
     * @param config 服务配置，未配置项使用默认值。
     * @return 启动成功返回 true；任一步失败返回 false 并回滚。
     * @note 重复调用安全，已启动时直接返回 true；不应与 stop() 并发调用。
     */
    bool start(const P2PServiceConfig& config);

    /**
     * 停止服务并等待两个线程退出。
     *
     * @note 幂等；不能在回调或工作线程中调用。
     */
    void stop();

    /**
     * 判断服务是否在运行。
     *
     * @return 已启动且未停止返回 true。
     */
    bool isRunning() const;

    /**
     * 获取实际生效的节点标识。
     *
     * @return 配置非 0 时与其一致，配置为 0 时为随机生成值；未启动返回 0。
     */
    std::uint32_t nodeId() const;

    /**
     * 获取实际数据端口。
     *
     * @return 配置固定端口时与其一致，配置为 0 时为系统分配值；未启动返回 0。
     */
    std::uint16_t dataPort() const;

    /**
     * 获取已发现节点快照，线程安全。
     *
     * @return 当前所有已发现节点；未启动返回空列表。
     */
    std::vector<PeerInfo> peers() const;

    /**
     * 查询指定节点，线程安全。
     *
     * @param nodeId 节点标识。
     * @param peer 输出节点信息，仅在返回 true 时被写入。
     * @return 已发现返回 true。
     */
    bool findPeer(std::uint32_t nodeId, PeerInfo& peer) const;

    /**
     * 设置发现新节点时的回调。
     *
     * @param handler 回调对象，为空表示取消。
     * @note 可在启动前或运行中设置；运行中替换时内部加锁保护。
     */
    void setPeerHandler(PeerHandler handler);

    /**
     * 与指定节点建立连接。
     *
     * @param peerNodeId 对端节点标识，必须已在节点表中。
     * @return 请求已受理返回 true。
     * @note 连接异步建立，结果通过 isConnected 或后续流事件观察。
     */
    bool connect(std::uint32_t peerNodeId);

    /**
     * 判断连接是否已建立。
     *
     * @param peerNodeId 对端节点标识。
     * @return 已连接返回 true。
     */
    bool isConnected(std::uint32_t peerNodeId) const;

    /**
     * 在指定节点上开一条流。
     *
     * 立即返回分配的流标识，未连接时自动建连并在就绪后补发开流帧，
     * 结果通过 StreamHandler 通知。
     *
     * @param peerNodeId 对端节点标识，必须已在节点表中。
     * @return 流标识；服务未启动、对端未知或无法分配时返回 0。
     * @note 返回非 0 不代表流已打开，超限或拒绝都会以 Rejected 回调。
     */
    std::uint32_t openStream(std::uint32_t peerNodeId);

    /**
     * 关闭流。
     *
     * @param streamId 流标识。
     * @return 请求已投递返回 true；服务未启动或参数非法返回 false。
     */
    bool closeStream(std::uint32_t streamId);

    /**
     * 向流提交数据。
     *
     * @param streamId 流标识。
     * @param data 数据内容。
     * @param size 长度，不能超过 maxPayloadSize。
     * @return 请求已投递返回 true。
     * @note 返回 true 表示已受理，不表示对端已收到。
     */
    bool sendData(std::uint32_t streamId, const void* data, std::size_t size);

    /**
     * 判断流是否已打开。
     *
     * @param streamId 流标识。
     * @return 已打开返回 true。
     */
    bool isStreamOpen(std::uint32_t streamId) const;

    /**
     * 获取流快照，线程安全。
     *
     * @return 当前所有流；服务未启动返回空列表。
     */
    std::vector<StreamInfo> streams() const;

    /**
     * 设置流事件回调。
     *
     * @param handler 回调对象，为空表示取消。
     * @note 可在启动前或运行中设置。
     */
    void setStreamHandler(StreamHandler handler);

    /**
     * 设置数据交付回调。
     *
     * @param handler 回调对象，为空表示取消。
     * @note 可在启动前或运行中设置。
     */
    void setDataHandler(DataHandler handler);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace netproxy
