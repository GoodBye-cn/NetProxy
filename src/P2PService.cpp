#include "P2PService.h"

#include "Log.h"
#include "core/IoLoop.h"
#include "core/TaskLoop.h"
#include "node/NodeDiscovery.h"
#include "node/NodeManager.h"
#include "session/DataChannel.h"
#include "session/SessionManager.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <utility>

namespace netproxy {

namespace {

// 生成非 0 的随机节点标识。
std::uint32_t randomNodeId() {
    std::random_device device;
    std::mt19937 engine(device());
    std::uniform_int_distribution<std::uint32_t> distribution(1, 0xFFFFFFFFU);
    return distribution(engine);
}

// 内部事件转换为对外事件。
SessionEvent toPublicEvent(session::SessionManager::Event event) {
    switch (event) {
    case session::SessionManager::Event::Opened:
        return SessionEvent::Opened;
    case session::SessionManager::Event::Rejected:
        return SessionEvent::Rejected;
    case session::SessionManager::Event::Closed:
        return SessionEvent::Closed;
    }
    return SessionEvent::Closed;
}

} // namespace

struct P2PService::Impl {
    core::IoLoop io;
    core::TaskLoop business;

    mutable std::mutex nodeMutex;
    std::unique_ptr<session::DataChannel> channel;
    std::unique_ptr<node::NodeDiscovery> discovery;
    std::unique_ptr<node::NodeManager> node;
    std::unique_ptr<session::SessionManager> sessions;

    std::mutex handlerMutex;
    PeerHandler handler;
    SessionHandler sessionHandler;

    std::uint32_t nodeId = 0;
    std::uint16_t dataPort = 0;
    std::atomic<bool> running{false};

    // 把当前回调安装到节点管理器，调用方需持有 nodeMutex。
    void installPeerHandler() {
        if (!node) {
            return;
        }

        std::lock_guard<std::mutex> lock(handlerMutex);
        node->setPeerHandler(handler);
    }

    // 把当前会话回调安装到会话模块，调用方需持有 nodeMutex。
    void installSessionHandler() {
        if (!sessions) {
            return;
        }

        SessionHandler target;
        {
            std::lock_guard<std::mutex> lock(handlerMutex);
            target = sessionHandler;
        }

        sessions->setHandler(
            [target](std::uint32_t sessionId, std::uint32_t peerNodeId, session::SessionManager::Event event) {
                if (target) {
                    target(sessionId, peerNodeId, toPublicEvent(event));
                }
            });
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

    node::NodeConfig nodeConfig;
    nodeConfig.announceIntervalMs = config.announceIntervalMs;
    nodeConfig.peerTimeoutMs = config.peerTimeoutMs;

    session::SessionConfig sessionConfig;
    sessionConfig.maxSessions = config.maxSessions;
    sessionConfig.maxSessionsPerPeer = config.maxSessionsPerPeer;
    sessionConfig.openRetryIntervalMs = config.sessionRetryIntervalMs;
    sessionConfig.openRetryCount = config.sessionRetryCount;

    std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);

    // 1. 数据通道先就绪，得到真实端口
    impl_->channel = std::make_unique<session::DataChannel>(impl_->io);
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
        std::make_unique<node::NodeManager>(*impl_->discovery, nodeId, nodeConfig, impl_->io, impl_->business);
    impl_->installPeerHandler();

    if (!impl_->node->start()) {
        impl_->node.reset();
        impl_->discovery.reset();
        impl_->channel.reset();
        return false;
    }

    // 4. 会话依赖数据通道与节点查询
    auto resolver = [node = impl_->node.get()](std::uint32_t peerNodeId, netproxy::PeerInfo& peer) {
        return node->findPeer(peerNodeId, peer);
    };

    impl_->sessions =
        std::make_unique<session::SessionManager>(*impl_->channel, resolver, nodeId, sessionConfig, impl_->business);
    impl_->installSessionHandler();

    if (!impl_->sessions->start()) {
        impl_->sessions.reset();
        impl_->node->stop();
        impl_->node.reset();
        impl_->discovery.reset();
        impl_->channel.reset();
        return false;
    }

    if (!impl_->io.start()) {
        impl_->sessions->stop();
        impl_->sessions.reset();
        impl_->node->stop();
        impl_->node.reset();
        impl_->discovery.reset();
        impl_->channel.reset();
        return false;
    }

    if (!impl_->business.start()) {
        impl_->io.stop();
        impl_->sessions->stop();
        impl_->sessions.reset();
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

    if (impl_->sessions) {
        impl_->sessions->stop();
        impl_->sessions.reset();
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

bool P2PService::findPeer(std::uint32_t nodeId, PeerInfo& peer) const {
    std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
    if (!impl_->node) {
        return false;
    }
    return impl_->node->findPeer(nodeId, peer);
}

void P2PService::setPeerHandler(PeerHandler handler) {
    std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
    {
        std::lock_guard<std::mutex> handlerLock(impl_->handlerMutex);
        impl_->handler = std::move(handler);
    }
    impl_->installPeerHandler();
}

std::uint32_t P2PService::openSession(std::uint32_t peerNodeId) {
    session::SessionManager* sessions = nullptr;
    {
        std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
        sessions = impl_->sessions.get();
    }

    if (sessions == nullptr) {
        return 0;
    }

    return sessions->open(peerNodeId);
}

bool P2PService::closeSession(std::uint32_t sessionId) {
    session::SessionManager* sessions = nullptr;
    {
        std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
        sessions = impl_->sessions.get();
    }

    if (sessions == nullptr) {
        return false;
    }

    return sessions->close(sessionId);
}

bool P2PService::isSessionEstablished(std::uint32_t sessionId) const {
    std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
    if (!impl_->sessions) {
        return false;
    }
    return impl_->sessions->isEstablished(sessionId);
}

std::vector<SessionInfo> P2PService::sessions() const {
    std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
    if (!impl_->sessions) {
        return std::vector<SessionInfo>();
    }

    std::vector<SessionInfo> result;
    for (const session::SessionRecord& record : impl_->sessions->list()) {
        SessionInfo info;
        info.sessionId = record.sessionId;
        info.peerNodeId = record.peerNodeId;
        info.established = record.state == session::SessionState::Established;
        result.push_back(info);
    }
    return result;
}

void P2PService::setSessionHandler(SessionHandler handler) {
    std::lock_guard<std::mutex> nodeLock(impl_->nodeMutex);
    {
        std::lock_guard<std::mutex> handlerLock(impl_->handlerMutex);
        impl_->sessionHandler = std::move(handler);
    }
    impl_->installSessionHandler();
}

} // namespace netproxy
