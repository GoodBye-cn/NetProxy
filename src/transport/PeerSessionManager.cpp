#include "PeerSessionManager.h"

#include "Log.h"
#include "transport/connection/PeerProtocol.h"

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

PeerSessionManager::PeerSessionManager(DataChannel& channel, std::uint32_t localNodeId,
                                             const PeerConfig& peerConfig, const StreamConfig& streamConfig,
                                             core::TaskLoop& business)
    : channel_(channel), localNodeId_(localNodeId), peerConfig_(peerConfig), streamConfig_(streamConfig),
      business_(business), sender_(channel.sender()) {}

PeerSessionManager::~PeerSessionManager() {
    stop();
}

bool PeerSessionManager::start() {
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

void PeerSessionManager::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    channel_.setReceiver(nullptr);

    std::lock_guard<std::mutex> lock(sessionsMutex_);
    sessions_.clear();
}

bool PeerSessionManager::connect(std::uint32_t peerNodeId, const net::IPAddress& peer) {
    if (!running_.load()) {
        return false;
    }

    business_.post([this, peerNodeId, peer] { ensureSession(peerNodeId, peer)->connect(); });
    return true;
}

std::uint32_t PeerSessionManager::openStream(std::uint32_t peerNodeId, const net::IPAddress& peer) {
    if (!running_.load()) {
        return 0;
    }

    // 流号在调用线程生成，保证公共 API 能同步返回（业务线程只负责建流）。
    const std::uint32_t streamId = allocateStreamId();

    business_.post([this, peerNodeId, peer, streamId] {
        auto session = ensureSession(peerNodeId, peer);
        if (!session->openStream(streamId)) {
            emitStreamEvent(streamId, peerNodeId, protocol::StreamEvent::Rejected, protocol::Reason::Error);
        }

        if (!session->isConnected()) {
            session->connect();
        }
    });

    return streamId;
}

bool PeerSessionManager::closeStream(std::uint32_t streamId) {
    if (!running_.load()) {
        return false;
    }

    business_.post([this, streamId] {
        auto session = sessionForStream(streamId);
        if (session) {
            session->closeStream(streamId);
        }
    });

    return true;
}

bool PeerSessionManager::sendData(std::uint32_t streamId, const void* data, std::size_t size) {
    if (!running_.load() || data == nullptr || size == 0) {
        return false;
    }

    std::string payload(static_cast<const char*>(data), size);
    business_.post([this, streamId, payload = std::move(payload)] {
        auto session = sessionForStream(streamId);
        if (session) {
            session->sendData(streamId, payload.data(), payload.size());
        }
    });

    return true;
}

bool PeerSessionManager::isConnected(std::uint32_t peerNodeId) const {
    std::shared_ptr<PeerSession> session;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        const auto entry = sessions_.find(peerNodeId);
        if (entry != sessions_.end()) {
            session = entry->second;
        }
    }

    return session && session->isConnected();
}

bool PeerSessionManager::isStreamOpen(std::uint32_t streamId) const {
    for (const auto& session : snapshotSessions()) {
        if (session->ownsStream(streamId)) {
            return session->isStreamOpen(streamId);
        }
    }

    return false;
}

std::vector<StreamRecord> PeerSessionManager::streams() const {
    std::vector<StreamRecord> result;

    for (const auto& session : snapshotSessions()) {
        std::vector<StreamRecord> records = session->streams();
        result.insert(result.end(), records.begin(), records.end());
    }

    return result;
}

void PeerSessionManager::setStreamHandler(StreamMux::StreamHandler handler) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    streamHandler_ = std::move(handler);
}

void PeerSessionManager::setDataHandler(StreamMux::DataHandler handler) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    dataHandler_ = std::move(handler);
}

void PeerSessionManager::onDatagram(const net::IPAddress& from, const char* data, std::size_t size) {
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
            ensureSession(message.nodeId, from)->acceptHandshake();

            HelloMessage ack;
            ack.type = static_cast<std::uint8_t>(protocol::HelloType::HelloAck);
            ack.nodeId = localNodeId_;
            ack.nonce = message.nonce;
            channel_.send(PeerProtocol::encodeHello(ack), from);
            return;
        }

        std::shared_ptr<PeerSession> session;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            const auto entry = sessions_.find(message.nodeId);
            if (entry != sessions_.end()) {
                session = entry->second;
            }
        }

        if (!session) {
            return;
        }

        if (message.type == static_cast<std::uint8_t>(protocol::HelloType::HelloAck)) {
            session->onHelloAck(message);
            return;
        }

        session->onHelloReject(message);
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        const auto entry = sessions_.find(message.nodeId);
        if (entry != sessions_.end() && entry->second == session) {
            sessions_.erase(entry);
        }
        return;
    }

    if (!isFrameType(type)) {
        return;
    }

    protocol::PeerFrame frame;
    if (!PeerProtocol::decodeFrame(data, size, frame)) {
        return;
    }

    std::shared_ptr<PeerSession> session;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        const auto entry = sessions_.find(frame.nodeId);
        if (entry != sessions_.end()) {
            session = entry->second;
        }
    }

    if (session) {
        session->onFrame(frame);
    }
}

void PeerSessionManager::tick() {
    if (!running_.load()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    for (const auto& session : snapshotSessions()) {
        session->tick(now);

        if (session->state() != ConnectionState::Closed) {
            continue;
        }

        // 会话删除统一在这里完成，避免流层回调里改路由表。
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        const auto entry = sessions_.find(session->peerNodeId());
        if (entry != sessions_.end() && entry->second == session) {
            sessions_.erase(entry);
        }
    }

    scheduleTick();
}

void PeerSessionManager::scheduleTick() {
    if (!running_.load()) {
        return;
    }

    business_.postDelayed(peerConfig_.tickMs, [this] { tick(); });
}

std::shared_ptr<PeerSession> PeerSessionManager::ensureSession(std::uint32_t peerNodeId,
                                                                 const net::IPAddress& peer) {
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        const auto entry = sessions_.find(peerNodeId);
        if (entry != sessions_.end()) {
            return entry->second;
        }
    }

    auto session =
        std::make_shared<PeerSession>(localNodeId_, peerNodeId, peer, peerConfig_, streamConfig_, sender_);

    // 事件转发不带会话指针：流层保证回调在锁外触发。
    session->setStreamHandler([this, peerNodeId](std::uint32_t streamId, std::uint32_t, protocol::StreamEvent event,
                                                 protocol::Reason reason) {
        emitStreamEvent(streamId, peerNodeId, event, reason);
    });

    session->setDataHandler([this, peerNodeId](std::uint32_t streamId, std::uint32_t, const char* data,
                                               std::size_t size) {
        emitData(streamId, peerNodeId, data, size);
    });

    std::lock_guard<std::mutex> lock(sessionsMutex_);
    const auto existing = sessions_.find(peerNodeId);
    if (existing != sessions_.end()) {
        return existing->second;
    }

    sessions_.emplace(peerNodeId, session);
    return session;
}

std::shared_ptr<PeerSession> PeerSessionManager::sessionForStream(std::uint32_t streamId) const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    for (const auto& entry : sessions_) {
        if (entry.second->ownsStream(streamId)) {
            return entry.second;
        }
    }

    return nullptr;
}

std::vector<std::shared_ptr<PeerSession>> PeerSessionManager::snapshotSessions() const {
    std::lock_guard<std::mutex> lock(sessionsMutex_);

    std::vector<std::shared_ptr<PeerSession>> result;
    result.reserve(sessions_.size());
    for (const auto& entry : sessions_) {
        result.push_back(entry.second);
    }
    return result;
}

void PeerSessionManager::emitStreamEvent(std::uint32_t streamId, std::uint32_t peerNodeId,
                                            protocol::StreamEvent event, protocol::Reason reason) {
    StreamMux::StreamHandler handler;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        handler = streamHandler_;
    }

    if (handler) {
        handler(streamId, peerNodeId, event, reason);
    }
}

void PeerSessionManager::emitData(std::uint32_t streamId, std::uint32_t peerNodeId, const char* data,
                                     std::size_t size) {
    StreamMux::DataHandler handler;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        handler = dataHandler_;
    }

    if (handler) {
        handler(streamId, peerNodeId, data, size);
    }
}

std::uint32_t PeerSessionManager::allocateStreamId() {
    return randomStreamId();
}

} // namespace netproxy::transport
