#include "PeerTable.h"

#include "Log.h"

namespace netproxy::node {

bool PeerTable::update(const PeerInfo& peer) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto entry = peers_.find(peer.nodeId);
    if (entry == peers_.end()) {
        PeerRecord record;
        record.info = peer;
        record.lastSeen = std::chrono::steady_clock::now();
        peers_.emplace(peer.nodeId, record);
        return true;
    }

    entry->second.info = peer;
    entry->second.lastSeen = std::chrono::steady_clock::now();
    return false;
}

bool PeerTable::remove(std::uint32_t nodeId) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto entry = peers_.find(nodeId);
    if (entry == peers_.end()) {
        return false;
    }

    Log::debug("peer removed nodeId=", nodeId, " ", entry->second.info.address, ":", entry->second.info.port);
    peers_.erase(entry);
    return true;
}

bool PeerTable::contains(std::uint32_t nodeId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peers_.find(nodeId) != peers_.end();
}

bool PeerTable::find(std::uint32_t nodeId, PeerInfo& peer) const {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto entry = peers_.find(nodeId);
    if (entry == peers_.end()) {
        return false;
    }

    peer = entry->second.info;
    return true;
}

std::vector<PeerInfo> PeerTable::list() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<PeerInfo> result;
    result.reserve(peers_.size());
    for (const auto& entry : peers_) {
        result.push_back(entry.second.info);
    }
    return result;
}

std::size_t PeerTable::removeExpired(std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto now = std::chrono::steady_clock::now();
    std::size_t removed = 0;

    for (auto entry = peers_.begin(); entry != peers_.end();) {
        if (now - entry->second.lastSeen > timeout) {
            Log::debug("expired peer removed nodeId=", entry->first, " ", entry->second.info.address, ":",
                       entry->second.info.port);
            entry = peers_.erase(entry);
            ++removed;
        } else {
            ++entry;
        }
    }

    if (removed > 0) {
        Log::debug("expired ", removed, " peers this round");
    }

    return removed;
}

std::size_t PeerTable::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peers_.size();
}

void PeerTable::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.clear();
}

} // namespace netproxy::node
