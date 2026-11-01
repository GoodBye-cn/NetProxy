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
std::string reasonPayload(protocol::CloseReason reason) {
    return std::string(1, static_cast<char>(reason));
}

} // namespace

PeerConnection::PeerConnection(std::uint32_t peerNodeId, std::uint32_t localNodeId, const net::IPAddress& peer,
                               const PeerConfig& config, Sender sender, HasStreams hasStreams)
    : peerNodeId_(peerNodeId), localNodeId_(localNodeId), peer_(peer), config_(config), sender_(std::move(sender)),
      hasStreams_(std::move(hasStreams)), peerWindow_(static_cast<std::uint32_t>(config.sendWindow)),
      lastActive_(std::chrono::steady_clock::now()) {}

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
    enterClosed(protocol::CloseReason::Error);
}

void PeerConnection::onFrame(const protocol::PeerFrame& frame) {
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
        enterClosed(protocol::CloseReason::Remote);
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
            close(protocol::CloseReason::Timeout);
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
                    close(protocol::CloseReason::Error);
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
                close(protocol::CloseReason::Timeout);
                return;
            }

            if (pingSentAt_ == std::chrono::steady_clock::time_point{} ||
                now - pingSentAt_ >= std::chrono::milliseconds(config_.pingTimeoutMs)) {
                sendControl(protocol::FrameType::Ping, std::string());
                pingSentAt_ = now;
                ++pingMisses_;
            }
        }

        // 流表在流层：只有「没有活跃流」且空闲超时才关闭连接。
        const bool hasActiveStreams = hasStreams_ ? hasStreams_() : false;
        if (!hasActiveStreams && now - lastActive_ >= std::chrono::milliseconds(config_.idleTimeoutMs)) {
            sendControl(protocol::FrameType::Close, reasonPayload(protocol::CloseReason::Local));
            state_ = ConnectionState::Closing;
            closingSince_ = now;
        }
        return;
    }
    case ConnectionState::Closing: {
        if (now - closingSince_ >= std::chrono::milliseconds(config_.closeTimeoutMs)) {
            enterClosed(protocol::CloseReason::Timeout);
        }
        return;
    }
    case ConnectionState::Closed:
        return;
    }
}

bool PeerConnection::send(protocol::PeerFrame frame) {
    if (state_ != ConnectionState::Connected) {
        return false;
    }

    if (frame.payload.size() > config_.maxPayloadSize) {
        return false;
    }

    queueFrame(std::move(frame));
    return true;
}

void PeerConnection::close(protocol::CloseReason reason) {
    if (state_ == ConnectionState::Closed) {
        return;
    }

    sendControl(protocol::FrameType::Close, reasonPayload(reason));
    enterClosed(reason);
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

const net::IPAddress& PeerConnection::peer() const {
    return peer_;
}

std::size_t PeerConnection::maxPayloadSize() const {
    return config_.maxPayloadSize;
}

void PeerConnection::setFrameHandler(FrameHandler handler) {
    frameHandler_ = std::move(handler);
}

void PeerConnection::setCloseHandler(CloseHandler handler) {
    closeHandler_ = std::move(handler);
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

void PeerConnection::queueFrame(protocol::PeerFrame frame) {
    const std::uint32_t limit =
        static_cast<std::uint32_t>(std::min(config_.sendWindow, static_cast<std::size_t>(peerWindow_)));
    if (sendNext_ - sendBase_ < limit) {
        sendFrame(std::move(frame));
    } else {
        pendingFrames_.push_back(std::move(frame));
    }
}

void PeerConnection::sendFrame(protocol::PeerFrame frame) {
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

        protocol::PeerFrame frame = std::move(pendingFrames_.front());
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

void PeerConnection::receiveFrame(const protocol::PeerFrame& frame) {
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

    deliverOrdered(frame);
    ++receiveNext_;

    while (true) {
        const auto entry = receiveBuffer_.find(receiveNext_);
        if (entry == receiveBuffer_.end()) {
            break;
        }

        deliverOrdered(entry->second);
        receiveBuffer_.erase(entry);
        ++receiveNext_;
    }

    sendAck();
}

void PeerConnection::deliverOrdered(const protocol::PeerFrame& frame) {
    if (frameHandler_) {
        frameHandler_(frame);
    }
}

void PeerConnection::sendAck() {
    sendControl(protocol::FrameType::Ping, std::string());
}

void PeerConnection::sendControl(protocol::FrameType type, const std::string& payload) {
    protocol::PeerFrame frame;
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

void PeerConnection::enterClosed(protocol::CloseReason reason) {
    if (state_ == ConnectionState::Closed) {
        return;
    }

    state_ = ConnectionState::Closed;
    sendQueue_.clear();
    pendingFrames_.clear();
    receiveBuffer_.clear();

    // 回调只触发一次；流层据此广播流关闭。
    if (closeHandler_) {
        closeHandler_(reason);
    }
}

std::uint16_t PeerConnection::windowRemaining() const {
    if (receiveBuffer_.size() >= config_.receiveWindow) {
        return 0;
    }
    return static_cast<std::uint16_t>(config_.receiveWindow - receiveBuffer_.size());
}

} // namespace netproxy::transport
