#include "PeerConnectionManager.h"

#include "Log.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <utility>

namespace netproxy::transport {

namespace {

// 生成非 0 流标识。
std::uint32_t randomStreamId() {
    thread_local std::mt19937 engine(std::random_device{}());
    std::uniform_int_distribution<std::uint32_t> distribution(1, 0xFFFFFFFFU);
    return distribution(engine);
}

bool isHelloType(std::uint8_t type) {
    return type >= static_cast<std::uint8_t>(protocol::HelloType::Hello) &&
           type <= static_cast<std::uint8_t>(protocol::HelloType::HelloReject);
}

bool isFrameType(std::uint8_t type) {
    return type >= static_cast<std::uint8_t>(protocol::FrameType::Data) &&
           type <= static_cast<std::uint8_t>(protocol::FrameType::Close);
}

} // namespace

PeerConnectionManager::PeerConnectionManager(DataChannel& channel, std::uint32_t localNodeId, const PeerConfig& config,
                                             core::TaskLoop& business)
    : channel_(channel), localNodeId_(localNodeId), config_(config), business_(business), sender_(channel.sender()) {}

PeerConnectionManager::~PeerConnectionManager() {
    stop();
}

bool PeerConnectionManager::start() {
    if (running_.load()) {
        return true;
    }

    const bool registered = channel_.setReceiver([this](const net::IPAddress& from, const char* data,
                                                        std::size_t size) {
        std::string packet(data, size);
        business_.post([this, from, packet = std::move(packet)] { onDatagram(from, packet.data(), packet.size()); });
    });

    if (!registered) {
        Log::error("连接管理启动失败: 无法注册数据通道回调");
        return false;
    }

    running_ = true;
    scheduleTick();
    return true;
}

void PeerConnectionManager::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    channel_.setReceiver(nullptr);
    connections_.clear();
    pendingStreams_.clear();

    std::lock_guard<std::mutex> lock(stateMutex_);
    connectedCache_.clear();
    streamToPeer_.clear();
    streamOpen_.clear();
}

bool PeerConnectionManager::connect(std::uint32_t peerNodeId, const net::IPAddress& peer) {
    if (!running_.load()) {
        return false;
    }

    business_.post([this, peerNodeId, peer] {
        auto connection = ensureConnection(peerNodeId, peer);
        connection->open();
    });
    return true;
}

std::uint32_t PeerConnectionManager::openStream(std::uint32_t peerNodeId, const net::IPAddress& peer) {
    if (!running_.load()) {
        return 0;
    }

    const std::uint32_t streamId = randomStreamId();

    business_.post([this, peerNodeId, peer, streamId] {
        auto connection = ensureConnection(peerNodeId, peer);
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            streamToPeer_[streamId] = peerNodeId;
            streamOpen_[streamId] = false;
        }

        if (connection->isConnected()) {
            if (!connection->openStream(streamId)) {
                removeStream(streamId);
                emitStreamEvent(streamId, peerNodeId, protocol::StreamEvent::Rejected, protocol::Reason::Error);
            }
            return;
        }

        pendingStreams_[peerNodeId].push_back(streamId);
        connection->open();
    });

    return streamId;
}

bool PeerConnectionManager::closeStream(std::uint32_t streamId) {
    if (!running_.load()) {
        return false;
    }

    business_.post([this, streamId] {
        const std::uint32_t peerNodeId = peerForStream(streamId);
        if (peerNodeId == 0) {
            return;
        }

        const auto pending = pendingStreams_.find(peerNodeId);
        if (pending != pendingStreams_.end()) {
            std::vector<std::uint32_t>& list = pending->second;
            const auto entry = std::find(list.begin(), list.end(), streamId);
            if (entry != list.end()) {
                list.erase(entry);
                removeStream(streamId);
                emitStreamEvent(streamId, peerNodeId, protocol::StreamEvent::Closed, protocol::Reason::Local);
                return;
            }
        }

        const auto connection = connections_.find(peerNodeId);
        if (connection == connections_.end()) {
            removeStream(streamId);
            return;
        }

        connection->second->closeStream(streamId);
    });

    return true;
}

bool PeerConnectionManager::sendData(std::uint32_t streamId, const void* data, std::size_t size) {
    if (!running_.load() || data == nullptr || size == 0) {
        return false;
    }

    std::string payload(static_cast<const char*>(data), size);
    business_.post([this, streamId, payload = std::move(payload)] {
        const std::uint32_t peerNodeId = peerForStream(streamId);
        if (peerNodeId == 0) {
            return;
        }

        const auto connection = connections_.find(peerNodeId);
        if (connection == connections_.end()) {
            return;
        }

        connection->second->sendData(streamId, payload.data(), payload.size());
    });

    return true;
}

bool PeerConnectionManager::isConnected(std::uint32_t peerNodeId) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto entry = connectedCache_.find(peerNodeId);
    return entry != connectedCache_.end() && entry->second;
}

void PeerConnectionManager::setStreamHandler(StreamHandler handler) {
    streamHandler_ = std::move(handler);
}

void PeerConnectionManager::setDataHandler(DataHandler handler) {
    dataHandler_ = std::move(handler);
}

void PeerConnectionManager::onDatagram(const net::IPAddress& from, const char* data, std::size_t size) {
    if (!running_.load() || size < 6) {
        return;
    }

    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    const std::uint8_t type = bytes[5];

    if (isHelloType(type)) {
        HelloMessage message;
        if (!PeerProtocol::decodeHello(data, size, message)) {
            return;
        }

        if (message.type == static_cast<std::uint8_t>(protocol::HelloType::Hello)) {
            auto connection = ensureConnection(message.nodeId, from);
            connection->acceptHandshake();

            HelloMessage ack;
            ack.type = static_cast<std::uint8_t>(protocol::HelloType::HelloAck);
            ack.nodeId = localNodeId_;
            ack.nonce = message.nonce;
            channel_.send(PeerProtocol::encodeHello(ack), from);

            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                connectedCache_[message.nodeId] = true;
            }
            flushPending(message.nodeId);
            return;
        }

        if (message.type == static_cast<std::uint8_t>(protocol::HelloType::HelloAck)) {
            const auto connection = connections_.find(message.nodeId);
            if (connection == connections_.end()) {
                return;
            }

            connection->second->onHelloAck(message);
            if (connection->second->isConnected()) {
                {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    connectedCache_[message.nodeId] = true;
                }
                flushPending(message.nodeId);
            }
            return;
        }

        const auto connection = connections_.find(message.nodeId);
        if (connection != connections_.end()) {
            connection->second->onHelloReject(message);
            connections_.erase(connection);
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            connectedCache_.erase(message.nodeId);
        }
        return;
    }

    if (!isFrameType(type)) {
        return;
    }

    PeerFrame frame;
    if (!PeerProtocol::decodeFrame(data, size, frame)) {
        return;
    }

    const auto connection = connections_.find(frame.nodeId);
    if (connection == connections_.end()) {
        return;
    }

    connection->second->onFrame(frame);
}

void PeerConnectionManager::tick() {
    if (!running_.load()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    for (auto entry = connections_.begin(); entry != connections_.end();) {
        entry->second->tick(now);

        if (entry->second->state() != ConnectionState::Closed) {
            ++entry;
            continue;
        }

        const std::uint32_t peerNodeId = entry->first;

        const auto pending = pendingStreams_.find(peerNodeId);
        if (pending != pendingStreams_.end()) {
            for (std::uint32_t streamId : pending->second) {
                removeStream(streamId);
                emitStreamEvent(streamId, peerNodeId, protocol::StreamEvent::Rejected, protocol::Reason::Timeout);
            }
            pendingStreams_.erase(pending);
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            connectedCache_.erase(peerNodeId);
        }

        entry = connections_.erase(entry);
    }

    scheduleTick();
}

void PeerConnectionManager::scheduleTick() {
    if (!running_.load()) {
        return;
    }

    business_.postDelayed(config_.tickMs, [this] { tick(); });
}

std::shared_ptr<PeerConnection> PeerConnectionManager::ensureConnection(std::uint32_t peerNodeId,
                                                                        const net::IPAddress& peer) {
    const auto existing = connections_.find(peerNodeId);
    if (existing != connections_.end()) {
        return existing->second;
    }

    auto connection = std::make_shared<PeerConnection>(peerNodeId, localNodeId_, peer, config_, sender_);

    connection->setStreamHandler(
        [this, peerNodeId](std::uint32_t streamId, protocol::StreamEvent event, protocol::Reason reason) {
            emitStreamEvent(streamId, peerNodeId, event, reason);
        });

    connection->setDataHandler([this, peerNodeId](std::uint32_t streamId, const char* data, std::size_t size) {
        emitData(streamId, peerNodeId, data, size);
    });

    connections_.emplace(peerNodeId, connection);
    return connection;
}

void PeerConnectionManager::flushPending(std::uint32_t peerNodeId) {
    const auto pending = pendingStreams_.find(peerNodeId);
    if (pending == pendingStreams_.end() || pending->second.empty()) {
        return;
    }

    const auto connection = connections_.find(peerNodeId);
    if (connection == connections_.end() || !connection->second->isConnected()) {
        return;
    }

    std::vector<std::uint32_t> list = std::move(pending->second);
    pendingStreams_.erase(pending);

    for (std::uint32_t streamId : list) {
        if (!connection->second->openStream(streamId)) {
            removeStream(streamId);
            emitStreamEvent(streamId, peerNodeId, protocol::StreamEvent::Rejected, protocol::Reason::Error);
        }
    }
}

void PeerConnectionManager::emitStreamEvent(std::uint32_t streamId, std::uint32_t peerNodeId,
                                            protocol::StreamEvent event, protocol::Reason reason) {
    if (event == protocol::StreamEvent::Opened) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        streamOpen_[streamId] = true;
    } else {
        removeStream(streamId);
    }

    if (streamHandler_) {
        streamHandler_(streamId, peerNodeId, event, reason);
    }
}

std::uint32_t PeerConnectionManager::peerForStream(std::uint32_t streamId) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto entry = streamToPeer_.find(streamId);
    return entry == streamToPeer_.end() ? 0 : entry->second;
}

void PeerConnectionManager::removeStream(std::uint32_t streamId) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    streamToPeer_.erase(streamId);
    streamOpen_.erase(streamId);
}

bool PeerConnectionManager::isStreamOpen(std::uint32_t streamId) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    const auto entry = streamOpen_.find(streamId);
    return entry != streamOpen_.end() && entry->second;
}

std::vector<StreamRecord> PeerConnectionManager::streams() const {
    std::lock_guard<std::mutex> lock(stateMutex_);

    std::vector<StreamRecord> result;
    result.reserve(streamToPeer_.size());
    for (const auto& entry : streamToPeer_) {
        StreamRecord record;
        record.streamId = entry.first;
        record.peerNodeId = entry.second;

        const auto open = streamOpen_.find(entry.first);
        record.open = open != streamOpen_.end() && open->second;
        result.push_back(record);
    }
    return result;
}

void PeerConnectionManager::emitData(std::uint32_t streamId, std::uint32_t peerNodeId, const char* data,
                                     std::size_t size) {
    if (dataHandler_) {
        dataHandler_(streamId, peerNodeId, data, size);
    }
}

std::uint32_t PeerConnectionManager::allocateStreamId() {
    return randomStreamId();
}

} // namespace netproxy::transport
