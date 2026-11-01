#include "P2PService.h"

#include "Log.h"
#include "core/IoLoop.h"
#include "core/TaskLoop.h"
#include "net/Socket.h"
#include "node/NodeDiscovery.h"
#include "node/PeerDirectory.h"
#include "protocol/Protocol.h"
#include "transport/DataChannel.h"
#include "transport/PeerSessionManager.h"

#include <atomic>
#include <mutex>
#include <random>
#include <utility>

namespace netproxy {

namespace {

// 生成非 0 的随机节点标识。
std::uint32_t randomNodeId() {
    thread_local std::mt19937 engine(std::random_device{}());
    std::uniform_int_distribution<std::uint32_t> distribution(1, 0xFFFFFFFFU);
    return distribution(engine);
}

// 内部流事件转换为对外事件。
StreamEvent toPublicEvent(protocol::StreamEvent event) {
    switch (event) {
    case protocol::StreamEvent::Opened:
        return StreamEvent::Opened;
    case protocol::StreamEvent::Rejected:
        return StreamEvent::Rejected;
    case protocol::StreamEvent::Closed:
        return StreamEvent::Closed;
    }
    return StreamEvent::Closed;
}

} // namespace

struct P2PService::Impl {
    core::IoLoop io;
    core::TaskLoop business;

    mutable std::mutex nodeMutex;
    std::unique_ptr<transport::DataChannel> channel;
    std::unique_ptr<node::NodeDiscovery> discovery;
    std::unique_ptr<node::PeerDirectory> node;
    std::unique_ptr<transport::PeerSessionManager> connections;

    std::mutex handlerMutex;
    PeerHandler handler;
    StreamHandler streamHandler;
    DataHandler dataHandler;

    std::uint32_t nodeId = 0;
    std::uint16_t dataPort = 0;
    std::atomic<bool> running{false};

    // 把当前发现回调安装到节点管理器，调用方需持有 nodeMutex。
    void installPeerHandler() {
        if (!node) {
            return;
        }

        PeerHandler target;
        {
            std::lock_guard<std::mutex> lock(handlerMutex);
            target = handler;
        }
        node->setPeerHandler(target);
    }
};

P2PService::P2PService() : impl_(std::make_unique<Impl>()) {}

P2PService::~P2PService() {
    stop();
}

bool P2PService::start(const P2PServiceConfig& config) {
    if (impl_->running.load()) {
        return true;
    }

    const std::uint32_t nodeId = config.nodeId == 0 ? randomNodeId() : config.nodeId;

    node::DiscoveryConfig nodeConfig;
    nodeConfig.announceIntervalMs = config.announceIntervalMs;
    nodeConfig.peerTimeoutMs = config.peerTimeoutMs;

    transport::PeerConfig peerConfig;
    peerConfig.helloRetryIntervalMs = config.helloRetryIntervalMs;
    peerConfig.helloRetryCount = config.helloRetryCount;
    peerConfig.maxPayloadSize = config.maxPayloadSize;
    peerConfig.sendWindow = config.sendWindow;
    peerConfig.receiveWindow = config.receiveWindow;
    peerConfig.retransmitTimeoutMs = config.retransmitTimeoutMs;
    peerConfig.maxRetransmitCount = config.maxRetransmitCount;
    peerConfig.pingIntervalMs = config.pingIntervalMs;
    peerConfig.pingTimeoutMs = config.pingTimeoutMs;
    peerConfig.pingRetryCount = config.pingRetryCount;

    transport::StreamConfig streamConfig;
    streamConfig.maxStreams = config.maxStreamsPerPeer;

    std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);

    // 1. 数据通道先就绪，得到真实端口
    impl_->channel = std::make_unique<transport::DataChannel>(impl_->io);
    if (!impl_->channel->open(config.dataBindAddress, config.dataPort)) {
        impl_->channel.reset();
        return false;
    }

    // 2. 发现通道用该端口公告
    impl_->discovery =
        std::make_unique<node::NodeDiscovery>(impl_->io, nodeId, config.multicastGroup, config.multicastPort);
    if (!impl_->discovery->open(impl_->channel->localPort())) {
        impl_->discovery.reset();
        impl_->channel.reset();
        return false;
    }

    // 3. 节点管理器只订阅公告
    impl_->node =
        std::make_unique<node::PeerDirectory>(*impl_->discovery, nodeId, nodeConfig, impl_->io, impl_->business);
    impl_->installPeerHandler();

    if (!impl_->node->start()) {
        impl_->node.reset();
        impl_->discovery.reset();
        impl_->channel.reset();
        return false;
    }

    // 4. 连接与流
    impl_->connections = std::make_unique<transport::PeerSessionManager>(*impl_->channel, nodeId, peerConfig,
                                                                         streamConfig, impl_->business);

    Impl* impl = impl_.get();
    impl_->connections->setStreamHandler([impl](std::uint32_t streamId, std::uint32_t peerNodeId,
                                                protocol::StreamEvent event, protocol::CloseReason reason) {
        StreamHandler target;
        {
            std::lock_guard<std::mutex> lock(impl->handlerMutex);
            target = impl->streamHandler;
        }

        if (target) {
            target(streamId, peerNodeId, toPublicEvent(event), static_cast<std::uint8_t>(reason));
        }
    });

    impl_->connections->setDataHandler(
        [impl](std::uint32_t streamId, std::uint32_t peerNodeId, const char* data, std::size_t size) {
            DataHandler target;
            {
                std::lock_guard<std::mutex> lock(impl->handlerMutex);
                target = impl->dataHandler;
            }

            if (target) {
                target(streamId, peerNodeId, data, size);
            }
        });

    if (!impl_->connections->start()) {
        impl_->connections.reset();
        impl_->node->stop();
        impl_->node.reset();
        impl_->discovery.reset();
        impl_->channel.reset();
        return false;
    }

    if (!impl_->io.start()) {
        impl_->connections->stop();
        impl_->connections.reset();
        impl_->node->stop();
        impl_->node.reset();
        impl_->discovery.reset();
        impl_->channel.reset();
        return false;
    }

    if (!impl_->business.start()) {
        impl_->io.stop();
        impl_->connections->stop();
        impl_->connections.reset();
        impl_->node->stop();
        impl_->node.reset();
        impl_->discovery.reset();
        impl_->channel.reset();
        return false;
    }

    impl_->nodeId = nodeId;
    impl_->dataPort = impl_->channel->localPort();
    impl_->running = true;

    Log::info("P2P 服务已启动 nodeId=", impl_->nodeId, " dataPort=", impl_->dataPort);
    return true;
}

void P2PService::stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }

    impl_->business.stop();
    impl_->io.stop();

    std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);

    if (impl_->connections) {
        impl_->connections->stop();
        impl_->connections.reset();
    }

    if (impl_->node) {
        impl_->node->stop();
        impl_->node.reset();
    }

    if (impl_->discovery) {
        impl_->discovery->close();
        impl_->discovery.reset();
    }

    if (impl_->channel) {
        impl_->channel->close();
        impl_->channel.reset();
    }

    Log::info("P2P 服务已停止");
}

bool P2PService::isRunning() const {
    return impl_->running.load();
}

std::uint32_t P2PService::nodeId() const {
    return impl_->nodeId;
}

std::uint16_t P2PService::dataPort() const {
    return impl_->dataPort;
}

std::vector<PeerInfo> P2PService::peers() const {
    std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
    if (!impl_->node) {
        return std::vector<PeerInfo>();
    }
    return impl_->node->peers();
}

bool P2PService::findPeer(std::uint32_t peerNodeId, PeerInfo& peer) const {
    std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
    if (!impl_->node) {
        return false;
    }
    return impl_->node->findPeer(peerNodeId, peer);
}

void P2PService::setPeerHandler(PeerHandler handler) {
    std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
    {
        std::lock_guard<std::mutex> handlerLock(impl_->handlerMutex);
        impl_->handler = std::move(handler);
    }
    impl_->installPeerHandler();
}

bool P2PService::connect(std::uint32_t peerNodeId) {
    net::IPAddress address;
    transport::PeerSessionManager* connections = nullptr;

    {
        std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
        if (!impl_->connections || !impl_->node) {
            return false;
        }

        PeerInfo peer;
        if (!impl_->node->findPeer(peerNodeId, peer)) {
            return false;
        }

        address = net::IPAddress::parse(peer.address, peer.port);
        if (!address.isValid()) {
            return false;
        }

        connections = impl_->connections.get();
    }

    return connections->connect(peerNodeId, address);
}

bool P2PService::isConnected(std::uint32_t peerNodeId) const {
    std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
    if (!impl_->connections) {
        return false;
    }
    return impl_->connections->isConnected(peerNodeId);
}

std::uint32_t P2PService::openStream(std::uint32_t peerNodeId) {
    net::IPAddress address;
    transport::PeerSessionManager* connections = nullptr;

    {
        std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
        if (!impl_->connections || !impl_->node) {
            return 0;
        }

        PeerInfo peer;
        if (!impl_->node->findPeer(peerNodeId, peer)) {
            return 0;
        }

        address = net::IPAddress::parse(peer.address, peer.port);
        if (!address.isValid()) {
            return 0;
        }

        connections = impl_->connections.get();
    }

    return connections->openStream(peerNodeId, address);
}

bool P2PService::closeStream(std::uint32_t streamId) {
    transport::PeerSessionManager* connections = nullptr;
    {
        std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
        connections = impl_->connections.get();
    }

    if (connections == nullptr) {
        return false;
    }
    return connections->closeStream(streamId);
}

bool P2PService::sendData(std::uint32_t streamId, const void* data, std::size_t size) {
    transport::PeerSessionManager* connections = nullptr;
    {
        std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
        connections = impl_->connections.get();
    }

    if (connections == nullptr) {
        return false;
    }
    return connections->sendData(streamId, data, size);
}

bool P2PService::isStreamOpen(std::uint32_t streamId) const {
    std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
    if (!impl_->connections) {
        return false;
    }
    return impl_->connections->isStreamOpen(streamId);
}

std::vector<StreamInfo> P2PService::streams() const {
    std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
    if (!impl_->connections) {
        return std::vector<StreamInfo>();
    }

    std::vector<StreamInfo> result;
    for (const transport::StreamRecord& record : impl_->connections->streams()) {
        StreamInfo info;
        info.streamId = record.streamId;
        info.peerNodeId = record.peerNodeId;
        info.open = record.open;
        result.push_back(info);
    }
    return result;
}

void P2PService::setStreamHandler(StreamHandler handler) {
    std::lock_guard<std::mutex> handlerLock(impl_->handlerMutex);
    impl_->streamHandler = std::move(handler);
}

void P2PService::setDataHandler(DataHandler handler) {
    std::lock_guard<std::mutex> handlerLock(impl_->handlerMutex);
    impl_->dataHandler = std::move(handler);
}

} // namespace netproxy
