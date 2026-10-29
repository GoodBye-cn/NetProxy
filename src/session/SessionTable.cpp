#include "SessionTable.h"

namespace netproxy::session {

bool SessionTable::insert(const SessionRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.emplace(record.sessionId, record).second;
}

bool SessionTable::find(std::uint32_t sessionId, SessionRecord& record) const {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto entry = sessions_.find(sessionId);
    if (entry == sessions_.end()) {
        return false;
    }

    record = entry->second;
    return true;
}

bool SessionTable::update(const SessionRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto entry = sessions_.find(record.sessionId);
    if (entry == sessions_.end()) {
        return false;
    }

    entry->second = record;
    return true;
}

bool SessionTable::remove(std::uint32_t sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.erase(sessionId) > 0;
}

std::size_t SessionTable::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

std::size_t SessionTable::countForPeer(std::uint32_t peerNodeId) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::size_t total = 0;
    for (const auto& entry : sessions_) {
        if (entry.second.peerNodeId == peerNodeId) {
            ++total;
        }
    }
    return total;
}

std::size_t SessionTable::removeExpired(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::size_t removed = 0;
    for (auto entry = sessions_.begin(); entry != sessions_.end();) {
        const SessionRecord& record = entry->second;
        const bool intermediate = record.state == SessionState::Opening || record.state == SessionState::Closing;
        if (intermediate && record.deadline <= now) {
            entry = sessions_.erase(entry);
            ++removed;
        } else {
            ++entry;
        }
    }
    return removed;
}

std::vector<SessionRecord> SessionTable::list() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<SessionRecord> result;
    result.reserve(sessions_.size());
    for (const auto& entry : sessions_) {
        result.push_back(entry.second);
    }
    return result;
}

void SessionTable::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.clear();
}

} // namespace netproxy::session
