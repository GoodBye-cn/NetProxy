#include "PeerSession.h"

#include <utility>

namespace netproxy::transport {

PeerSession::PeerSession(std::uint32_t localNodeId, std::uint32_t peerNodeId, const net::IPAddress& peer,
                         const PeerConfig& peerConfig, const StreamConfig& streamConfig, PacketSender sender)
    : connection_(peerNodeId, localNodeId, peer, peerConfig, sender, [this] { return streams_.hasStreams(); }),
      streams_(peerNodeId, streamConfig,
               [this](protocol::PeerFrame frame) { return connection_.send(std::move(frame)); }) {
    connection_.setFrameHandler([this](const protocol::PeerFrame& frame) { streams_.onFrame(frame); });

    connection_.setCloseHandler([this](protocol::CloseReason reason) {
        connected_.store(false);
        streams_.onConnectionClosed(reason);
    });
}

bool PeerSession::connect() {
    return connection_.open();
}

void PeerSession::acceptHandshake() {
    connection_.acceptHandshake();
    onHandshakeCompleted();
}

void PeerSession::onHelloAck(const HelloMessage& message) {
    connection_.onHelloAck(message);
    onHandshakeCompleted();
}

void PeerSession::onHelloReject(const HelloMessage& message) {
    connection_.onHelloReject(message);
}

void PeerSession::onFrame(const protocol::PeerFrame& frame) {
    connection_.onFrame(frame);
}

void PeerSession::tick(std::chrono::steady_clock::time_point now) {
    connection_.tick(now);
}

void PeerSession::close(protocol::CloseReason reason) {
    connection_.close(reason);
}

bool PeerSession::openStream(std::uint32_t streamId) {
    return streams_.openStream(streamId);
}

bool PeerSession::sendData(std::uint32_t streamId, const void* data, std::size_t size) {
    return streams_.sendData(streamId, data, size);
}

bool PeerSession::closeStream(std::uint32_t streamId) {
    return streams_.closeStream(streamId);
}

bool PeerSession::ownsStream(std::uint32_t streamId) const {
    return streams_.ownsStream(streamId);
}

bool PeerSession::isStreamOpen(std::uint32_t streamId) const {
    return streams_.isStreamOpen(streamId);
}

std::vector<StreamRecord> PeerSession::streams() const {
    return streams_.streams();
}

void PeerSession::setStreamHandler(StreamMux::StreamHandler handler) {
    streams_.setStreamHandler(std::move(handler));
}

void PeerSession::setDataHandler(StreamMux::DataHandler handler) {
    streams_.setDataHandler(std::move(handler));
}

std::uint32_t PeerSession::peerNodeId() const {
    return connection_.peerNodeId();
}

const net::IPAddress& PeerSession::peer() const {
    return connection_.peer();
}

bool PeerSession::isConnected() const {
    return connected_.load();
}

ConnectionState PeerSession::state() const {
    return connection_.state();
}

void PeerSession::onHandshakeCompleted() {
    if (!connection_.isConnected()) {
        return;
    }

    connected_.store(true);
    streams_.onConnectionUp();
}

} // namespace netproxy::transport
