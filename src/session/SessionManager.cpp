#include "SessionManager.h"

#include "Log.h"

#include <chrono>
#include <random>
#include <utility>

namespace netproxy::session {

namespace {

// 生成非 0 的会话标识，可在任意线程调用。
std::uint32_t randomSessionId() {
    thread_local std::mt19937 engine(std::random_device{}());
    std::uniform_int_distribution<std::uint32_t> distribution(1, 0xFFFFFFFFU);
    return distribution(engine);
}

// 生成握手验证码。
std::uint32_t randomNonce() {
    thread_local std::mt19937 engine(std::random_device{}());
    std::uniform_int_distribution<std::uint32_t> distribution(1, 0xFFFFFFFFU);
    return distribution(engine);
}

} // namespace

SessionManager::SessionManager(DataChannel& channel, PeerResolver resolver, std::uint32_t localNodeId,
                               const SessionConfig& config, core::TaskLoop& business)
    : config_(config), channel_(channel), resolver_(std::move(resolver)), localNodeId_(localNodeId),
      business_(business),
      streams_(std::make_unique<transport::DataStreamManager>(
          [this](const std::string& packet, const net::IPAddress& target) { channel_.send(packet, target); },
          localNodeId, config.transport)) {}

SessionManager::~SessionManager() {
    stop();
}

bool SessionManager::start() {
    if (running_.load()) {
        return true;
    }

    if (!channel_.isOpen()) {
        Log::error("会话模块启动失败: 数据通道未打开");
        return false;
    }

    const bool registered = channel_.setReceiver(
        [this](const net::IPAddress& from, const char* data, std::size_t size) { handleDatagram(from, data, size); });

    if (!registered) {
        Log::error("会话模块启动失败: 无法注册数据通道回调");
        return false;
    }

    streams_->setDeliveredHandler([this](std::uint32_t sessionId, const char* data, std::size_t size) {
        onDataDelivered(sessionId, data, size);
    });
    streams_->setClosedHandler(
        [this](std::uint32_t sessionId, protocol::CloseReason reason) { closeWithReason(sessionId, reason); });

    running_ = true;
    scheduleKeepalive();
    scheduleDataTick();
    Log::debug("会话模块已启动");
    return true;
}

void SessionManager::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    channel_.setReceiver(nullptr);
    table_.clear();
    streams_->clear();
    Log::debug("会话模块已停止");
}

bool SessionManager::isRunning() const {
    return running_.load();
}

std::uint32_t SessionManager::open(std::uint32_t peerNodeId) {
    if (!running_.load()) {
        Log::warning("会话建立失败: 模块未启动");
        return 0;
    }

    if (peerNodeId == 0) {
        Log::warning("会话建立失败: 节点标识无效");
        return 0;
    }

    const std::uint32_t sessionId = allocateSessionId();
    if (sessionId == 0) {
        Log::warning("会话建立失败: 无法分配会话标识");
        return 0;
    }

    business_.post([this, sessionId, peerNodeId] { beginOpen(sessionId, peerNodeId); });
    return sessionId;
}

std::uint32_t SessionManager::allocateSessionId() {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::uint32_t sessionId = randomSessionId();
        SessionRecord record;
        if (!table_.find(sessionId, record)) {
            return sessionId;
        }
    }
    return 0;
}

bool SessionManager::close(std::uint32_t sessionId) {
    if (!running_.load() || sessionId == 0) {
        return false;
    }

    business_.post([this, sessionId] { closeOnBusiness(sessionId); });
    return true;
}

bool SessionManager::isEstablished(std::uint32_t sessionId) const {
    SessionRecord record;
    return table_.find(sessionId, record) && record.state == SessionState::Established;
}

std::vector<SessionRecord> SessionManager::list() const {
    return table_.list();
}

bool SessionManager::send(std::uint32_t sessionId, const void* data, std::size_t size) {
    if (!running_.load() || data == nullptr || size == 0) {
        return false;
    }

    SessionRecord record;
    if (!table_.find(sessionId, record) || record.state != SessionState::Established) {
        return false;
    }

    std::string payload(static_cast<const char*>(data), size);
    business_.post(
        [this, sessionId, payload = std::move(payload)] { streams_->send(sessionId, payload.data(), payload.size()); });
    return true;
}

void SessionManager::setHandler(Handler handler) {
    std::lock_guard<std::mutex> lock(handlerMutex_);
    handler_ = std::move(handler);
}

void SessionManager::setDataHandler(DataHandler handler) {
    std::lock_guard<std::mutex> lock(handlerMutex_);
    dataHandler_ = std::move(handler);
}

void SessionManager::handleDatagram(const net::IPAddress& from, const char* data, std::size_t size) {
    // 数据报文交给传输模块，信令报文走会话状态机
    if (size > 5 && protocol::isDataMessage(static_cast<unsigned char>(data[5]))) {
        std::string packet(data, size);
        business_.post(
            [this, from, packet = std::move(packet)] { streams_->onDatagram(from, packet.data(), packet.size()); });
        return;
    }

    SessionMessage message;
    if (!SessionMessage::decode(data, size, message)) {
        return;
    }

    business_.post([this, from, message] { onMessage(from, message); });
}

void SessionManager::onMessage(const net::IPAddress& from, const SessionMessage& message) {
    touchSession(message.sessionId);

    switch (message.type) {
    case SessionType::Open:
        handleOpen(from, message);
        break;
    case SessionType::Accept:
        handleAccept(from, message);
        break;
    case SessionType::Reject:
        handleReject(message);
        break;
    case SessionType::Close:
        handleClose(message);
        break;
    case SessionType::Ping:
        handlePing(from, message);
        break;
    }
}

void SessionManager::beginOpen(std::uint32_t sessionId, std::uint32_t peerNodeId) {
    if (!running_.load()) {
        notify(sessionId, peerNodeId, Event::Rejected);
        return;
    }

    if (table_.count() >= config_.maxSessions) {
        Log::warning("会话建立失败: 超过总会话上限 ", config_.maxSessions);
        notify(sessionId, peerNodeId, Event::Rejected);
        return;
    }

    if (table_.countForPeer(peerNodeId) >= config_.maxSessionsPerPeer) {
        Log::warning("会话建立失败: 超过单节点会话上限 ", config_.maxSessionsPerPeer);
        notify(sessionId, peerNodeId, Event::Rejected);
        return;
    }

    netproxy::PeerInfo peer;
    if (!resolver_ || !resolver_(peerNodeId, peer)) {
        Log::warning("会话建立失败: 未知节点 ", peerNodeId);
        notify(sessionId, peerNodeId, Event::Rejected);
        return;
    }

    const net::IPAddress address = net::IPAddress::parse(peer.address, peer.port);
    if (!address.isValid()) {
        Log::warning("会话建立失败: 对端地址无效 ", peer.address, ":", peer.port);
        notify(sessionId, peerNodeId, Event::Rejected);
        return;
    }

    SessionRecord record;
    record.sessionId = sessionId;
    record.peerNodeId = peerNodeId;
    record.peerAddress = address;
    record.state = SessionState::Opening;
    record.nonce = randomNonce();
    record.lastActivity = std::chrono::steady_clock::now();
    record.deadline = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(config_.openRetryIntervalMs * (config_.openRetryCount + 1));

    if (!table_.insert(record)) {
        Log::warning("会话建立失败: 会话标识冲突");
        notify(sessionId, peerNodeId, Event::Rejected);
        return;
    }

    SessionMessage message;
    message.type = SessionType::Open;
    message.sessionId = record.sessionId;
    message.nodeId = localNodeId_;
    message.listenPort = channel_.localPort();
    message.nonce = record.nonce;
    sendMessage(message, address);

    scheduleRetry(record.sessionId);

    Log::debug("发起会话 sessionId=", record.sessionId, " peer=", peerNodeId);
}

bool SessionManager::closeOnBusiness(std::uint32_t sessionId) {
    SessionRecord record;
    if (!table_.find(sessionId, record)) {
        return false;
    }

    SessionMessage message;
    message.type = SessionType::Close;
    message.nodeId = localNodeId_;
    message.sessionId = sessionId;
    message.reason = static_cast<std::uint8_t>(CloseReason::Local);
    sendMessage(message, record.peerAddress);

    table_.remove(sessionId);
    streams_->remove(sessionId);

    Log::debug("会话已关闭 sessionId=", sessionId);
    notify(sessionId, record.peerNodeId, Event::Closed);
    return true;
}

void SessionManager::handleOpen(const net::IPAddress& from, const SessionMessage& message) {
    SessionRecord existing;
    if (table_.find(message.sessionId, existing)) {
        if (existing.state == SessionState::Established) {
            SessionMessage accept;
            accept.type = SessionType::Accept;
            accept.sessionId = message.sessionId;
            accept.nodeId = localNodeId_;
            accept.nonce = message.nonce;
            sendMessage(accept, from);
        }
        return;
    }

    if (table_.count() >= config_.maxSessions) {
        sendReject(message.sessionId, RejectReason::TooManySessions, from);
        return;
    }

    if (table_.countForPeer(message.nodeId) >= config_.maxSessionsPerPeer) {
        sendReject(message.sessionId, RejectReason::TooManyPerPeer, from);
        return;
    }

    netproxy::PeerInfo peer;
    if (!resolver_ || !resolver_(message.nodeId, peer)) {
        sendReject(message.sessionId, RejectReason::UnknownPeer, from);
        return;
    }

    SessionRecord record;
    record.sessionId = message.sessionId;
    record.peerNodeId = message.nodeId;
    record.peerAddress = from;
    record.state = SessionState::Established;
    record.nonce = message.nonce;
    record.lastActivity = std::chrono::steady_clock::now();
    table_.insert(record);
    streams_->create(record.sessionId, record.peerNodeId, record.peerAddress);

    SessionMessage accept;
    accept.type = SessionType::Accept;
    accept.sessionId = message.sessionId;
    accept.nodeId = localNodeId_;
    accept.nonce = message.nonce;
    sendMessage(accept, from);

    Log::debug("会话已建立 sessionId=", message.sessionId, " peer=", message.nodeId);
    notify(message.sessionId, message.nodeId, Event::Opened);
}

void SessionManager::handleAccept(const net::IPAddress& from, const SessionMessage& message) {
    SessionRecord record;
    if (!table_.find(message.sessionId, record)) {
        Log::warning("收到未知会话的 Accept sessionId=", message.sessionId);
        return;
    }

    if (record.state != SessionState::Opening) {
        return;
    }

    if (message.nodeId != record.peerNodeId) {
        Log::warning("Accept 节点不匹配 sessionId=", message.sessionId);
        return;
    }

    if (message.nonce != record.nonce) {
        Log::warning("Accept 验证码不匹配 sessionId=", message.sessionId);
        return;
    }

    record.state = SessionState::Established;
    record.retryCount = 0;
    if (from.isValid()) {
        record.peerAddress = from;
    }
    table_.update(record);
    streams_->create(record.sessionId, record.peerNodeId, record.peerAddress);

    Log::debug("会话已建立 sessionId=", record.sessionId, " peer=", record.peerNodeId);
    notify(record.sessionId, record.peerNodeId, Event::Opened);
}

void SessionManager::handleReject(const SessionMessage& message) {
    SessionRecord record;
    if (!table_.find(message.sessionId, record) || record.state != SessionState::Opening) {
        return;
    }

    if (message.nodeId != record.peerNodeId) {
        Log::warning("Reject 节点不匹配 sessionId=", message.sessionId);
        return;
    }

    table_.remove(message.sessionId);

    Log::warning("会话被拒绝 sessionId=", message.sessionId, " reason=", static_cast<int>(message.reason));
    notify(message.sessionId, record.peerNodeId, Event::Rejected);
}

void SessionManager::handleClose(const SessionMessage& message) {
    SessionRecord record;
    if (!table_.find(message.sessionId, record)) {
        return;
    }

    if (message.nodeId != record.peerNodeId) {
        Log::warning("Close 节点不匹配 sessionId=", message.sessionId);
        return;
    }

    table_.remove(message.sessionId);
    streams_->remove(message.sessionId);

    Log::debug("会话已关闭 sessionId=", message.sessionId);
    notify(message.sessionId, record.peerNodeId, Event::Closed);
}

void SessionManager::sendMessage(const SessionMessage& message, const net::IPAddress& target) {
    channel_.send(message.encode(), target);
}

void SessionManager::scheduleRetry(std::uint32_t sessionId) {
    business_.postDelayed(config_.openRetryIntervalMs, [this, sessionId] { retry(sessionId); });
}

void SessionManager::retry(std::uint32_t sessionId) {
    if (!running_.load()) {
        return;
    }

    SessionRecord record;
    if (!table_.find(sessionId, record) || record.state != SessionState::Opening) {
        return;
    }

    if (record.retryCount >= config_.openRetryCount) {
        table_.remove(sessionId);
        Log::warning("会话握手超时 sessionId=", sessionId);
        notify(sessionId, record.peerNodeId, Event::Rejected);
        return;
    }

    ++record.retryCount;
    record.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.openRetryIntervalMs);
    table_.update(record);

    SessionMessage message;
    message.type = SessionType::Open;
    message.sessionId = record.sessionId;
    message.nodeId = localNodeId_;
    message.listenPort = channel_.localPort();
    message.nonce = record.nonce;
    sendMessage(message, record.peerAddress);

    scheduleRetry(sessionId);
}

void SessionManager::sendReject(std::uint32_t sessionId, RejectReason reason, const net::IPAddress& target) {
    SessionMessage message;
    message.type = SessionType::Reject;
    message.nodeId = localNodeId_;
    message.sessionId = sessionId;
    message.reason = static_cast<std::uint8_t>(reason);
    sendMessage(message, target);
}

void SessionManager::handlePing(const net::IPAddress& from, const SessionMessage& message) {
    SessionRecord record;
    if (!table_.find(message.sessionId, record)) {
        return;
    }

    // 响应不再回复，避免两端互相回包形成循环
    if ((message.flags & SESSION_FLAG_RESPONSE) != 0) {
        return;
    }

    SessionMessage reply;
    reply.type = SessionType::Ping;
    reply.flags = SESSION_FLAG_RESPONSE;
    reply.sessionId = message.sessionId;
    reply.nodeId = localNodeId_;
    sendMessage(reply, from);
}

void SessionManager::touchSession(std::uint32_t sessionId) {
    SessionRecord record;
    if (!table_.find(sessionId, record)) {
        return;
    }

    record.lastActivity = std::chrono::steady_clock::now();
    record.keepaliveMisses = 0;
    table_.update(record);
}

void SessionManager::tickKeepalive() {
    if (!running_.load()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    for (const SessionRecord& snapshot : table_.list()) {
        if (snapshot.state != SessionState::Established) {
            continue;
        }

        SessionRecord record = snapshot;

        if (record.keepaliveMisses > 0) {
            if (now - record.pingSentAt < std::chrono::milliseconds(config_.keepaliveTimeoutMs)) {
                continue;
            }

            if (record.keepaliveMisses >= config_.keepaliveRetryCount) {
                Log::warning("会话保活超时 sessionId=", record.sessionId, " peer=", record.peerNodeId);

                SessionMessage close;
                close.type = SessionType::Close;
                close.sessionId = record.sessionId;
                close.nodeId = localNodeId_;
                close.reason = static_cast<std::uint8_t>(CloseReason::Timeout);
                sendMessage(close, record.peerAddress);

                table_.remove(record.sessionId);
                streams_->remove(record.sessionId);
                notify(record.sessionId, record.peerNodeId, Event::Closed);
                continue;
            }
        } else if (now - record.lastActivity < std::chrono::milliseconds(config_.keepaliveIntervalMs)) {
            continue;
        }

        SessionMessage ping;
        ping.type = SessionType::Ping;
        ping.sessionId = record.sessionId;
        ping.nodeId = localNodeId_;
        sendMessage(ping, record.peerAddress);

        record.pingSentAt = now;
        ++record.keepaliveMisses;
        table_.update(record);
    }
}

void SessionManager::scheduleKeepalive() {
    if (!running_.load()) {
        return;
    }

    business_.postDelayed(config_.keepaliveTickMs, [this] {
        if (!running_.load()) {
            return;
        }

        tickKeepalive();
        scheduleKeepalive();
    });
}

void SessionManager::tickData() {
    if (!running_.load()) {
        return;
    }

    streams_->tick();
    scheduleDataTick();
}

void SessionManager::scheduleDataTick() {
    if (!running_.load()) {
        return;
    }

    business_.postDelayed(config_.transport.dataTickMs, [this] { tickData(); });
}

void SessionManager::onDataDelivered(std::uint32_t sessionId, const char* data, std::size_t size) {
    touchSession(sessionId);

    DataHandler handler;
    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        handler = dataHandler_;
    }

    SessionRecord record;
    const std::uint32_t peerNodeId = table_.find(sessionId, record) ? record.peerNodeId : 0;
    if (handler) {
        handler(sessionId, peerNodeId, data, size);
    }
}

void SessionManager::closeWithReason(std::uint32_t sessionId, CloseReason reason) {
    SessionRecord record;
    if (!table_.find(sessionId, record)) {
        return;
    }

    SessionMessage close;
    close.type = SessionType::Close;
    close.sessionId = sessionId;
    close.nodeId = localNodeId_;
    close.reason = static_cast<std::uint8_t>(reason);
    sendMessage(close, record.peerAddress);

    table_.remove(sessionId);
    streams_->remove(sessionId);

    Log::warning("会话被动关闭 sessionId=", sessionId, " reason=", static_cast<int>(reason));
    notify(sessionId, record.peerNodeId, Event::Closed);
}

void SessionManager::notify(std::uint32_t sessionId, std::uint32_t peerNodeId, Event event) {
    Handler handler;
    {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        handler = handler_;
    }

    if (handler) {
        handler(sessionId, peerNodeId, event);
    }
}

} // namespace netproxy::session
