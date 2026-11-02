#include "PeerDirectory.h"

#include "Log.h"
#include "core/Bind.h"

#include <functional>
#include <utility>

namespace netproxy::node {

PeerDirectory::PeerDirectory(NodeDiscovery& discovery, std::uint32_t nodeId, const DiscoveryConfig& config,
                             core::IoLoop& io, core::TaskLoop& business)
    : config_(config), discovery_(discovery), nodeId_(nodeId), io_(io), business_(business) {}

PeerDirectory::~PeerDirectory() {
    stop();
}

bool PeerDirectory::start() {
    if (running_.load()) {
        return true;
    }

    if (!discovery_.isOpen()) {
        Log::error("peer directory start failed: discovery channel not open");
        return false;
    }

    const bool registered = discovery_.setReceiver(core::bindFront<&PeerDirectory::onPeerSeen>(this));

    if (!registered) {
        Log::error("peer directory start failed: cannot register discovery callback");
        return false;
    }

    running_ = true;
    lastAnnounce_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(config_.announceIntervalMs);
    scheduleTick();

    Log::debug("peer directory started nodeId=", nodeId_);
    return true;
}

void PeerDirectory::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    Log::debug("peer directory stopped nodeId=", nodeId_);
}

std::uint32_t PeerDirectory::nodeId() const {
    return nodeId_;
}

std::vector<PeerInfo> PeerDirectory::peers() const {
    return peers_.list();
}

bool PeerDirectory::findPeer(std::uint32_t peerNodeId, PeerInfo& peer) const {
    return peers_.find(peerNodeId, peer);
}

void PeerDirectory::setPeerHandler(PeerHandler handler) {
    std::lock_guard<std::mutex> lock(handlerMutex_);
    handler_ = std::move(handler);
}

void PeerDirectory::onPeerSeen(const PeerInfo& peer) {
    business_.post(std::bind(&PeerDirectory::onPeerAnnounced, this, peer));
}

void PeerDirectory::onPeerAnnounced(const PeerInfo& peer) {
    if (!peers_.update(peer)) {
        return;
    }

    Log::debug("peer discovered nodeId=", peer.nodeId, " ", peer.address, ":", peer.port);

    PeerHandler handler;
    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        handler = handler_;
    }

    if (handler) {
        handler(peer);
    }
}

void PeerDirectory::announceOnIoThread() {
    Log::debug("multicast announce nodeId=", nodeId_, " port=", discovery_.announcePort());
    discovery_.announce();
}

void PeerDirectory::tick() {
    const auto now = std::chrono::steady_clock::now();

    if (now - lastAnnounce_ >= std::chrono::milliseconds(config_.announceIntervalMs)) {
        lastAnnounce_ = now;
        io_.post(std::bind(&PeerDirectory::announceOnIoThread, this));
    }

    peers_.removeExpired(std::chrono::milliseconds(config_.peerTimeoutMs));
}

void PeerDirectory::onTickTimer() {
    if (!running_.load()) {
        return;
    }

    tick();
    scheduleTick();
}

void PeerDirectory::scheduleTick() {
    if (!running_.load()) {
        return;
    }

    business_.postDelayed(config_.tickIntervalMs, std::bind(&PeerDirectory::onTickTimer, this));
}

} // namespace netproxy::node
