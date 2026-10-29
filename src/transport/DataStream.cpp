#include "DataStream.h"

#include <algorithm>
#include <utility>

namespace netproxy::transport {

DataStream::DataStream(std::uint32_t sessionId, std::uint32_t peerNodeId, const net::IPAddress& peer,
                       std::uint32_t localNodeId, const TransportConfig& config, Sender sender)
    : sessionId_(sessionId), peerNodeId_(peerNodeId), localNodeId_(localNodeId), peer_(peer), config_(config),
      sender_(std::move(sender)) {}

DataStream::~DataStream() = default;

bool DataStream::send(const void* data, std::size_t size) {
    if (closed_ || data == nullptr || size == 0 || size > config_.maxPayloadSize) {
        return false;
    }

    std::string payload(static_cast<const char*>(data), size);

    const std::uint32_t limit =
        static_cast<std::uint32_t>(std::min(config_.sendWindow, static_cast<std::size_t>(peerWindow_)));
    if (sendNext_ - sendBase_ < limit) {
        PendingSegment segment;
        segment.sequence = sendNext_++;
        segment.payload = std::move(payload);
        sendQueue_.push_back(segment);
        sendSegment(sendQueue_.back());
    } else {
        pendingPayloads_.push_back(std::move(payload));
    }

    return true;
}

void DataStream::onData(const DataMessage& message) {
    if (closed_) {
        return;
    }

    // Data 报文同样承载确认信息
    onAck(message);

    if (message.sequence == receiveNext_) {
        if (delivered_) {
            delivered_(message.payload.data(), message.payload.size());
        }
        ++receiveNext_;
        deliverContiguous();
    } else if (message.sequence > receiveNext_ && receiveBuffer_.size() < config_.receiveWindow) {
        receiveBuffer_.emplace(message.sequence, message.payload);
    }

    sendAck();
}

void DataStream::onAck(const DataMessage& message) {
    if (closed_) {
        return;
    }

    const std::uint32_t acknowledge = message.acknowledge;

    if (acknowledge > sendNext_) {
        return;
    }

    if (acknowledge > sendBase_) {
        while (!sendQueue_.empty() && sendQueue_.front().sequence < acknowledge) {
            sendQueue_.pop_front();
        }

        sendBase_ = acknowledge;
        duplicateAcks_ = 0;
        peerWindow_ = message.window;
        fillWindow();
        return;
    }

    if (acknowledge == sendBase_ && !sendQueue_.empty()) {
        ++duplicateAcks_;
        if (duplicateAcks_ >= 3) {
            duplicateAcks_ = 0;
            PendingSegment& head = sendQueue_.front();
            ++head.retransmitCount;
            sendSegment(head);
        }
    }
}

void DataStream::tick(std::chrono::steady_clock::time_point now) {
    if (closed_ || sendQueue_.empty()) {
        return;
    }

    PendingSegment& head = sendQueue_.front();
    if (now - head.sentAt < std::chrono::milliseconds(config_.retransmitTimeoutMs)) {
        return;
    }

    if (head.retransmitCount >= config_.maxRetransmitCount) {
        close(protocol::CloseReason::Error);
        return;
    }

    ++head.retransmitCount;
    sendSegment(head);
}

void DataStream::setDeliveredHandler(Delivered handler) {
    delivered_ = std::move(handler);
}

void DataStream::setClosedHandler(Closed handler) {
    closedHandler_ = std::move(handler);
}

std::uint32_t DataStream::sessionId() const {
    return sessionId_;
}

std::uint32_t DataStream::peerNodeId() const {
    return peerNodeId_;
}

bool DataStream::isClosed() const {
    return closed_;
}

void DataStream::sendSegment(PendingSegment& segment) {
    segment.sentAt = std::chrono::steady_clock::now();

    DataMessage message;
    message.type = static_cast<std::uint8_t>(protocol::MessageType::Data);
    message.sessionId = sessionId_;
    message.nodeId = localNodeId_;
    message.sequence = segment.sequence;
    message.acknowledge = receiveNext_;
    message.window = windowRemaining();
    message.payload = segment.payload;

    if (sender_) {
        sender_(message.encodePacket(), peer_);
    }
}

void DataStream::sendAck() {
    DataMessage message;
    message.type = static_cast<std::uint8_t>(protocol::MessageType::Ack);
    message.sessionId = sessionId_;
    message.nodeId = localNodeId_;
    message.acknowledge = receiveNext_;
    message.window = windowRemaining();

    if (sender_) {
        sender_(message.encodePacket(), peer_);
    }
}

void DataStream::deliverContiguous() {
    while (true) {
        const auto entry = receiveBuffer_.find(receiveNext_);
        if (entry == receiveBuffer_.end()) {
            break;
        }

        std::string payload = std::move(entry->second);
        receiveBuffer_.erase(entry);

        if (delivered_) {
            delivered_(payload.data(), payload.size());
        }
        ++receiveNext_;
    }
}

void DataStream::fillWindow() {
    while (!pendingPayloads_.empty()) {
        const std::uint32_t limit =
            static_cast<std::uint32_t>(std::min(config_.sendWindow, static_cast<std::size_t>(peerWindow_)));
        if (sendNext_ - sendBase_ >= limit) {
            break;
        }

        PendingSegment segment;
        segment.sequence = sendNext_++;
        segment.payload = std::move(pendingPayloads_.front());
        pendingPayloads_.pop_front();
        sendQueue_.push_back(segment);
        sendSegment(sendQueue_.back());
    }
}

void DataStream::close(protocol::CloseReason reason) {
    if (closed_) {
        return;
    }

    closed_ = true;
    sendQueue_.clear();
    pendingPayloads_.clear();
    receiveBuffer_.clear();

    auto handler = std::move(closedHandler_);
    closedHandler_ = Closed();
    if (handler) {
        handler(reason);
    }
}

std::uint16_t DataStream::windowRemaining() const {
    if (receiveBuffer_.size() >= config_.receiveWindow) {
        return 0;
    }
    return static_cast<std::uint16_t>(config_.receiveWindow - receiveBuffer_.size());
}

} // namespace netproxy::transport
