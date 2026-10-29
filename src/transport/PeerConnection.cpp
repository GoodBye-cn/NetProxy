#include "PeerConnection.h"

#include <algorithm>
#include <random>
#include <utility>

namespace netproxy::transport {

namespace {

// 生成非 0 握手随机数。
std::uint32_t randomNonce() {
    thread_local std::mt19937 engine(std::random_device{}());
    std::uniform_int_distribution<std::uint32_t> distribution(1, 0xFFFFFFFFU);
    return distribution(engine);
}

// 关闭原因放入 1 字节负载。
std::string reasonPayload(protocol::Reason reason) {
    return std::string(1, static_cast<char>(reason));
}

} // namespace

PeerConnection::PeerConnection(std::uint32_t peerNodeId, std::uint32_t localNodeId, const net::IPAddress& peer,
                               const PeerConfig& config, Sender sender)
    : peerNodeId_(peerNodeId), localNodeId_(localNodeId), peer_(peer), config_(config), sender_(std::move(sender)),
      peerWindow_(static_cast<std::uint32_t>(config.sendWindow)), lastActive_(std::chrono::steady_clock::now()) {}

PeerConnection::~PeerConnection() = default;

bool PeerConnection::open() {
    if (state_ != ConnectionState::Connecting || handshakeStarted_) {
        return false;
    }

    handshakeStarted_ = true;
    helloNonce_ = randomNonce();
    helloRetries_ = 0;
    sendHello();
    return true;
}

void PeerConnection::onHelloAck(const HelloMessage& message) {
    if (state_ != ConnectionState::Connecting) {
        return;
    }

    if (message.nonce != helloNonce_) {
        return;
    }

    state_ = ConnectionState::Connected;
    lastActive_ = std::chrono::steady_clock::now();
    flushWindow();
}

void PeerConnection::acceptHandshake() {
    if (state_ != ConnectionState::Connecting) {
        return;
    }

    state_ = ConnectionState::Connected;
    lastActive_ = std::chrono::steady_clock::now();
}

void PeerConnection::onHelloReject(const HelloMessage& message) {
    (void)message;
    if (state_ == ConnectionState::Closed) {
        return;
    }

    state_ = ConnectionState::Closed;
    closeAllStreams(protocol::Reason::Error);
}

void PeerConnection::onFrame(const PeerFrame& frame) {
    if (state_ != ConnectionState::Connected && state_ != ConnectionState::Closing) {
        return;
    }

    lastActive_ = std::chrono::steady_clock::now();
    pingMisses_ = 0;

    handleAck(frame.acknowledge, frame.window);

    const auto type = static_cast<protocol::FrameType>(frame.type);
    if (type == protocol::FrameType::Ping) {
        return; // 纯 ACK 或心跳，不参与有序交付
    }
    if (type == protocol::FrameType::Close) {
        deliverFrame(frame);
        return;
    }

    receiveFrame(frame);
}

void PeerConnection::tick(std::chrono::steady_clock::time_point now) {
    switch (state_) {
    case ConnectionState::Connecting: {
        if (now - helloSentAt_ < std::chrono::milliseconds(config_.helloRetryIntervalMs)) {
            return;
        }

        if (helloRetries_ >= config_.helloRetryCount) {
            close(protocol::Reason::Timeout);
            return;
        }

        ++helloRetries_;
        sendHello();
        return;
    }
    case ConnectionState::Connected: {
        if (!sendQueue_.empty()) {
            PendingFrame& head = sendQueue_.front();
            if (now - head.sentAt >= std::chrono::milliseconds(config_.retransmitTimeoutMs)) {
                if (head.retransmitCount >= config_.maxRetransmitCount) {
                    close(protocol::Reason::Error);
                    return;
                }

                ++head.retransmitCount;
                head.sentAt = now;
                if (sender_) {
                    sender_(PeerProtocol::encodeFrame(head.frame), peer_);
                }
            }
        }

        if (now - lastActive_ >= std::chrono::milliseconds(config_.pingIntervalMs)) {
            if (pingMisses_ >= config_.pingRetryCount) {
                close(protocol::Reason::Timeout);
                return;
            }

            if (pingSentAt_ == std::chrono::steady_clock::time_point{} ||
                now - pingSentAt_ >= std::chrono::milliseconds(config_.pingTimeoutMs)) {
                sendControl(protocol::FrameType::Ping, std::string());
                pingSentAt_ = now;
                ++pingMisses_;
            }
        }

        if (streams_.empty() && now - lastActive_ >= std::chrono::milliseconds(config_.idleTimeoutMs)) {
            sendControl(protocol::FrameType::Close, reasonPayload(protocol::Reason::Local));
            state_ = ConnectionState::Closing;
            closingSince_ = now;
        }
        return;
    }
    case ConnectionState::Closing: {
        if (now - closingSince_ >= std::chrono::milliseconds(config_.closeTimeoutMs)) {
            state_ = ConnectionState::Closed;
            closeAllStreams(protocol::Reason::Timeout);
        }
        return;
    }
    case ConnectionState::Closed:
        return;
    }
}

bool PeerConnection::openStream(std::uint32_t streamId) {
    if (state_ != ConnectionState::Connected || streamId == 0) {
        return false;
    }

    if (streams_.find(streamId) != streams_.end() || streams_.size() >= config_.maxStreams) {
        return false;
    }

    streams_.emplace(streamId, StreamState::Opening);

    PeerFrame frame;
    frame.type = static_cast<std::uint8_t>(protocol::FrameType::StreamOpen);
    frame.streamId = streamId;
    queueFrame(frame);
    return true;
}

bool PeerConnection::closeStream(std::uint32_t streamId) {
    const auto entry = streams_.find(streamId);
    if (entry == streams_.end()) {
        return false;
    }

    streams_.erase(entry);

    PeerFrame frame;
    frame.type = static_cast<std::uint8_t>(protocol::FrameType::StreamClose);
    frame.streamId = streamId;
    frame.payload = reasonPayload(protocol::Reason::Local);
    queueFrame(frame);

    emitStreamEvent(streamId, protocol::StreamEvent::Closed, protocol::Reason::Local);
    return true;
}

bool PeerConnection::sendData(std::uint32_t streamId, const void* data, std::size_t size) {
    if (state_ != ConnectionState::Connected || data == nullptr || size == 0 || size > config_.maxPayloadSize) {
        return false;
    }

    const auto entry = streams_.find(streamId);
    if (entry == streams_.end() || entry->second != StreamState::Open) {
        return false;
    }

    PeerFrame frame;
    frame.type = static_cast<std::uint8_t>(protocol::FrameType::Data);
    frame.streamId = streamId;
    frame.payload.assign(static_cast<const char*>(data), size);
    queueFrame(frame);
    return true;
}

void PeerConnection::close(protocol::Reason reason) {
    if (state_ == ConnectionState::Closed) {
        return;
    }

    sendControl(protocol::FrameType::Close, reasonPayload(reason));
    state_ = ConnectionState::Closed;
    sendQueue_.clear();
    pendingFrames_.clear();
    receiveBuffer_.clear();
    closeAllStreams(reason);
}

bool PeerConnection::isConnected() const {
    return state_ == ConnectionState::Connected;
}

ConnectionState PeerConnection::state() const {
    return state_;
}

std::uint32_t PeerConnection::peerNodeId() const {
    return peerNodeId_;
}

void PeerConnection::setStreamHandler(StreamCallback handler) {
    streamHandler_ = std::move(handler);
}

void PeerConnection::setDataHandler(DataCallback handler) {
    dataHandler_ = std::move(handler);
}

void PeerConnection::sendHello() {
    HelloMessage message;
    message.type = static_cast<std::uint8_t>(protocol::HelloType::Hello);
    message.nodeId = localNodeId_;
    message.nonce = helloNonce_;

    if (sender_) {
        sender_(PeerProtocol::encodeHello(message), peer_);
    }
    helloSentAt_ = std::chrono::steady_clock::now();
}

void PeerConnection::queueFrame(PeerFrame frame) {
    const std::uint32_t limit =
        static_cast<std::uint32_t>(std::min(config_.sendWindow, static_cast<std::size_t>(peerWindow_)));
    if (sendNext_ - sendBase_ < limit) {
        sendFrame(std::move(frame));
    } else {
        pendingFrames_.push_back(std::move(frame));
    }
}

void PeerConnection::sendFrame(PeerFrame frame) {
    frame.nodeId = localNodeId_;
    frame.connectionSeq = sendNext_++;
    frame.acknowledge = receiveNext_;
    frame.window = windowRemaining();

    PendingFrame pending;
    pending.frame = frame;
    pending.sentAt = std::chrono::steady_clock::now();
    pending.retransmitCount = 0;
    sendQueue_.push_back(std::move(pending));

    if (sender_) {
        sender_(PeerProtocol::encodeFrame(frame), peer_);
    }
}

void PeerConnection::flushWindow() {
    while (!pendingFrames_.empty()) {
        const std::uint32_t limit =
            static_cast<std::uint32_t>(std::min(config_.sendWindow, static_cast<std::size_t>(peerWindow_)));
        if (sendNext_ - sendBase_ >= limit) {
            break;
        }

        PeerFrame frame = std::move(pendingFrames_.front());
        pendingFrames_.pop_front();
        sendFrame(std::move(frame));
    }
}

void PeerConnection::handleAck(std::uint32_t acknowledge, std::uint16_t window) {
    if (acknowledge > sendNext_) {
        return;
    }

    if (acknowledge > sendBase_) {
        while (!sendQueue_.empty() && sendQueue_.front().frame.connectionSeq < acknowledge) {
            sendQueue_.pop_front();
        }

        sendBase_ = acknowledge;
        duplicateAcks_ = 0;
        peerWindow_ = window;
        flushWindow();
        return;
    }

    if (acknowledge == sendBase_ && !sendQueue_.empty()) {
        ++duplicateAcks_;
        if (duplicateAcks_ >= 3) {
            duplicateAcks_ = 0;
            PendingFrame& head = sendQueue_.front();
            ++head.retransmitCount;
            head.sentAt = std::chrono::steady_clock::now();
            if (sender_) {
                sender_(PeerProtocol::encodeFrame(head.frame), peer_);
            }
        }
    }
}

void PeerConnection::receiveFrame(const PeerFrame& frame) {
    if (frame.connectionSeq < receiveNext_) {
        sendAck();
        return;
    }

    if (frame.connectionSeq > receiveNext_) {
        if (receiveBuffer_.size() < config_.receiveWindow) {
            receiveBuffer_.emplace(frame.connectionSeq, frame);
        }
        sendAck();
        return;
    }

    deliverFrame(frame);
    ++receiveNext_;

    while (true) {
        const auto entry = receiveBuffer_.find(receiveNext_);
        if (entry == receiveBuffer_.end()) {
            break;
        }

        deliverFrame(entry->second);
        receiveBuffer_.erase(entry);
        ++receiveNext_;
    }

    sendAck();
}

void PeerConnection::deliverFrame(const PeerFrame& frame) {
    const auto type = static_cast<protocol::FrameType>(frame.type);

    switch (type) {
    case protocol::FrameType::Data:
        if (dataHandler_) {
            dataHandler_(frame.streamId, frame.payload.data(), frame.payload.size());
        }
        break;
    case protocol::FrameType::StreamOpen:
    case protocol::FrameType::StreamAccept:
    case protocol::FrameType::StreamReject:
    case protocol::FrameType::StreamClose:
        handleStreamFrame(frame);
        break;
    case protocol::FrameType::Ping:
        break;
    case protocol::FrameType::Close:
        state_ = ConnectionState::Closed;
        sendQueue_.clear();
        pendingFrames_.clear();
        closeAllStreams(protocol::Reason::Remote);
        break;
    }
}

void PeerConnection::handleStreamFrame(const PeerFrame& frame) {
    const auto type = static_cast<protocol::FrameType>(frame.type);

    if (type == protocol::FrameType::StreamOpen) {
        const auto existing = streams_.find(frame.streamId);
        if (existing != streams_.end()) {
            if (existing->second == StreamState::Open || existing->second == StreamState::Opening) {
                PeerFrame accept;
                accept.type = static_cast<std::uint8_t>(protocol::FrameType::StreamAccept);
                accept.streamId = frame.streamId;
                queueFrame(accept);
            }
            return;
        }

        if (streams_.size() >= config_.maxStreams) {
            PeerFrame reject;
            reject.type = static_cast<std::uint8_t>(protocol::FrameType::StreamReject);
            reject.streamId = frame.streamId;
            reject.payload = reasonPayload(protocol::Reason::Error);
            queueFrame(reject);
            return;
        }

        streams_.emplace(frame.streamId, StreamState::Open);

        PeerFrame accept;
        accept.type = static_cast<std::uint8_t>(protocol::FrameType::StreamAccept);
        accept.streamId = frame.streamId;
        queueFrame(accept);

        emitStreamEvent(frame.streamId, protocol::StreamEvent::Opened, static_cast<protocol::Reason>(0));
        return;
    }

    if (type == protocol::FrameType::StreamAccept) {
        const auto entry = streams_.find(frame.streamId);
        if (entry == streams_.end() || entry->second != StreamState::Opening) {
            return;
        }

        entry->second = StreamState::Open;
        emitStreamEvent(frame.streamId, protocol::StreamEvent::Opened, static_cast<protocol::Reason>(0));
        return;
    }

    if (type == protocol::FrameType::StreamReject) {
        const auto entry = streams_.find(frame.streamId);
        if (entry == streams_.end()) {
            return;
        }

        streams_.erase(entry);
        emitStreamEvent(frame.streamId, protocol::StreamEvent::Rejected, protocol::Reason::Error);
        return;
    }

    const auto entry = streams_.find(frame.streamId);
    if (entry == streams_.end()) {
        return;
    }

    streams_.erase(entry);
    emitStreamEvent(frame.streamId, protocol::StreamEvent::Closed, protocol::Reason::Remote);
}

void PeerConnection::sendAck() {
    sendControl(protocol::FrameType::Ping, std::string());
}

void PeerConnection::sendControl(protocol::FrameType type, const std::string& payload) {
    PeerFrame frame;
    frame.type = static_cast<std::uint8_t>(type);
    frame.nodeId = localNodeId_;
    frame.connectionSeq = 0; // 控制帧不占交付序号
    frame.acknowledge = receiveNext_;
    frame.window = windowRemaining();
    frame.streamId = 0;
    frame.payload = payload;

    if (sender_) {
        sender_(PeerProtocol::encodeFrame(frame), peer_);
    }
}

void PeerConnection::closeAllStreams(protocol::Reason reason) {
    if (streams_.empty()) {
        return;
    }

    for (const auto& entry : streams_) {
        emitStreamEvent(entry.first, protocol::StreamEvent::Closed, reason);
    }
    streams_.clear();
}

void PeerConnection::emitStreamEvent(std::uint32_t streamId, protocol::StreamEvent event, protocol::Reason reason) {
    if (streamHandler_) {
        streamHandler_(streamId, event, reason);
    }
}

std::uint16_t PeerConnection::windowRemaining() const {
    if (receiveBuffer_.size() >= config_.receiveWindow) {
        return 0;
    }
    return static_cast<std::uint16_t>(config_.receiveWindow - receiveBuffer_.size());
}

} // namespace netproxy::transport
