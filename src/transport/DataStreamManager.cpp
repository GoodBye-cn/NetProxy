#include "DataStreamManager.h"

#include <chrono>
#include <utility>
#include <vector>

namespace netproxy::transport {

DataStreamManager::DataStreamManager(Sender sender, std::uint32_t localNodeId, const TransportConfig& config)
    : localNodeId_(localNodeId), config_(config), sender_(std::move(sender)) {}

DataStreamManager::~DataStreamManager() = default;

void DataStreamManager::create(std::uint32_t sessionId, std::uint32_t peerNodeId, const net::IPAddress& peer) {
    if (streams_.find(sessionId) != streams_.end()) {
        return;
    }

    auto stream = std::make_shared<DataStream>(sessionId, peerNodeId, peer, localNodeId_, config_, sender_);

    stream->setDeliveredHandler([this, sessionId](const char* data, std::size_t size) {
        if (delivered_) {
            delivered_(sessionId, data, size);
        }
    });

    stream->setClosedHandler([this, sessionId](protocol::CloseReason reason) {
        if (closed_) {
            closed_(sessionId, reason);
        }
    });

    streams_.emplace(sessionId, std::move(stream));
}

void DataStreamManager::remove(std::uint32_t sessionId) {
    streams_.erase(sessionId);
}

bool DataStreamManager::send(std::uint32_t sessionId, const void* data, std::size_t size) {
    const auto entry = streams_.find(sessionId);
    if (entry == streams_.end()) {
        return false;
    }
    return entry->second->send(data, size);
}

void DataStreamManager::onDatagram(const net::IPAddress& from, const char* data, std::size_t size) {
    (void)from;

    DataMessage message;
    if (!DataMessage::decodePacket(data, size, message)) {
        return;
    }

    const auto entry = streams_.find(message.sessionId);
    if (entry == streams_.end()) {
        return;
    }

    if (message.type == static_cast<std::uint8_t>(protocol::MessageType::Data)) {
        entry->second->onData(message);
    } else {
        entry->second->onAck(message);
    }
}

void DataStreamManager::tick() {
    std::vector<std::uint32_t> ids;
    ids.reserve(streams_.size());
    for (const auto& entry : streams_) {
        ids.push_back(entry.first);
    }

    const auto now = std::chrono::steady_clock::now();
    for (std::uint32_t sessionId : ids) {
        const auto entry = streams_.find(sessionId);
        if (entry == streams_.end()) {
            continue;
        }

        // 保持一份引用：断链回调里可能把自身从表中移除
        std::shared_ptr<DataStream> stream = entry->second;
        stream->tick(now);
    }
}

void DataStreamManager::clear() {
    streams_.clear();
}

void DataStreamManager::setDeliveredHandler(Delivered handler) {
    delivered_ = std::move(handler);
}

void DataStreamManager::setClosedHandler(Closed handler) {
    closed_ = std::move(handler);
}

std::size_t DataStreamManager::size() const {
    return streams_.size();
}

} // namespace netproxy::transport
