#include "NodeManager.h"

#include "Log.h"

#include <utility>

namespace netproxy::node {

NodeManager::NodeManager(NodeDiscovery& discovery, std::uint32_t nodeId, const NodeConfig& config, core::IoLoop& io,
                         core::TaskLoop& business)
    : config_(config), discovery_(discovery), nodeId_(nodeId), io_(io), business_(business) {}

NodeManager::~NodeManager() {
    stop();
}

bool NodeManager::start() {
    if (running_.load()) {
        return true;
    }

    if (!discovery_.isOpen()) {
        Log::error("节点管理器启动失败: 发现通道未打开");
        return false;
    }

    const bool registered = discovery_.setReceiver(
        [this](const PeerInfo& peer) { business_.post([this, peer] { onPeerAnnounced(peer); }); });

    if (!registered) {
        Log::error("节点管理器启动失败: 无法注册发现回调");
        return false;
    }

    running_ = true;
    lastAnnounce_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(config_.announceIntervalMs);
    scheduleTick();

    Log::debug("节点管理器已启动 nodeId=", nodeId_);
    return true;
}

void NodeManager::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    Log::debug("节点管理器已停止 nodeId=", nodeId_);
}

std::uint32_t NodeManager::nodeId() const {
    return nodeId_;
}

std::vector<PeerInfo> NodeManager::peers() const {
    return peers_.list();
}

bool NodeManager::findPeer(std::uint32_t nodeId, PeerInfo& peer) const {
    return peers_.find(nodeId, peer);
}

void NodeManager::setPeerHandler(PeerHandler handler) {
    std::lock_guard<std::mutex> lock(handlerMutex_);
    handler_ = std::move(handler);
}

void NodeManager::onPeerAnnounced(const PeerInfo& peer) {
    if (!peers_.update(peer)) {
        return;
    }

    Log::debug("发现节点 nodeId=", peer.nodeId, " ", peer.address, ":", peer.port);

    PeerHandler handler;
    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        handler = handler_;
    }

    if (handler) {
        handler(peer);
    }
}

void NodeManager::tick() {
    const auto now = std::chrono::steady_clock::now();

    if (now - lastAnnounce_ >= std::chrono::milliseconds(config_.announceIntervalMs)) {
        lastAnnounce_ = now;
        io_.post([this] {
            Log::debug("组播公告 nodeId=", nodeId_, " port=", discovery_.announcePort());
            discovery_.announce();
        });
    }

    peers_.removeExpired(std::chrono::milliseconds(config_.peerTimeoutMs));
}

void NodeManager::scheduleTick() {
    if (!running_.load()) {
        return;
    }

    business_.postDelayed(config_.tickIntervalMs, [this] {
        if (!running_.load()) {
            return;
        }

        tick();
        scheduleTick();
    });
}

} // namespace netproxy::node
