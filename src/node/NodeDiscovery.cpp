#include "NodeDiscovery.h"

#include "DiscoveryProtocol.h"
#include "Log.h"
#include "core/Bind.h"

#include <utility>

namespace netproxy::node {

NodeDiscovery::NodeDiscovery(core::IoLoop& io, std::uint32_t nodeId, const std::string& group, std::uint16_t port)
    : io_(io), multicastGroup_(group.empty() ? DEFAULT_MULTICAST_GROUP : group),
      multicastPort_(port == 0 ? DEFAULT_MULTICAST_PORT : port), nodeId_(nodeId) {}

NodeDiscovery::~NodeDiscovery() {
    close();
}

bool NodeDiscovery::open(std::uint16_t announcePort) {
    if (opened_) {
        return true;
    }

    announcePort_ = announcePort;

    multicastAddress_ = net::IPAddress::parse(multicastGroup_, multicastPort_);
    if (!multicastAddress_.isValid() || multicastAddress_.family() != AF_INET) {
        Log::error("node discovery start failed: invalid multicast address ", multicastGroup_, ":", multicastPort_);
        return false;
    }

    const net::IPAddress bindAddress = net::IPAddress::any(multicastPort_);
    if (!socket_.open(bindAddress)) {
        Log::error("node discovery start failed: cannot create socket");
        return false;
    }

    // 多个实例共享同一组播端口时需要地址复用
    if (!socket_.setReuseAddress(true)) {
        Log::error("node discovery start failed: cannot enable address reuse");
        socket_.close();
        return false;
    }

    if (!socket_.bind(bindAddress)) {
        Log::error("node discovery start failed: cannot bind multicast port ", multicastPort_);
        socket_.close();
        return false;
    }

    if (!socket_.joinMulticastGroup(multicastGroup_)) {
        Log::error("node discovery start failed: cannot join multicast group ", multicastGroup_);
        socket_.close();
        return false;
    }

    if (!socket_.setMulticastLoop(true) || !socket_.setMulticastTtl(1)) {
        Log::error("node discovery start failed: cannot set multicast options");
        socket_.close();
        return false;
    }

    if (!socket_.setNonBlocking(true)) {
        socket_.close();
        return false;
    }

    opened_ = true;
    Log::debug("node discovery channel opened group=", multicastGroup_, ":", multicastPort_,
               " announcePort=", announcePort_);
    return true;
}

void NodeDiscovery::close() {
    if (!opened_) {
        return;
    }

    if (registered_) {
        io_.dispatcher().remove(socket_.nativeHandle());
        registered_ = false;
    }

    socket_.close();
    opened_ = false;
}

bool NodeDiscovery::isOpen() const {
    return opened_;
}

bool NodeDiscovery::announce() {
    return sendMessage(AnnounceType::Announce);
}

bool NodeDiscovery::sendBye() {
    return sendMessage(AnnounceType::Bye);
}

bool NodeDiscovery::setReceiver(Receiver receiver) {
    if (!opened_) {
        Log::warning("node discovery setReceiver failed: channel not open");
        return false;
    }

    receiver_ = std::move(receiver);

    if (registered_) {
        return true;
    }

    registered_ =
        io_.dispatcher().add(socket_.nativeHandle(), net::Poller::Read, core::bindFront<&NodeDiscovery::onEvent>(this));
    if (!registered_) {
        Log::error("node discovery setReceiver failed: cannot register read event");
    }
    return registered_;
}

std::uint16_t NodeDiscovery::announcePort() const {
    return announcePort_;
}

bool NodeDiscovery::sendMessage(AnnounceType type) {
    if (!opened_) {
        Log::warning("node discovery send failed: channel not open");
        return false;
    }

    AnnounceMessage message;
    message.type = type;
    message.nodeId = nodeId_;
    message.port = announcePort_;

    if (socket_.sendTo(message.encode(), multicastAddress_) <= 0) {
        Log::warning("node discovery send failed: ", type == AnnounceType::Announce ? "announce" : "bye");
        return false;
    }
    return true;
}

void NodeDiscovery::onEvent(net::SocketHandle, std::uint32_t) {
    while (true) {
        char buffer[64] = {};
        net::IPAddress from;
        const int received = socket_.receiveFrom(buffer, sizeof(buffer), from);
        if (received <= 0) {
            break;
        }

        AnnounceMessage message;
        if (!AnnounceMessage::decode(buffer, static_cast<std::size_t>(received), message)) {
            continue;
        }

        if (message.nodeId == nodeId_) {
            continue;
        }

        PeerInfo peer;
        // 报文声明的地址优先，未声明或非法时回退到源地址
        peer.address = message.address.isValid() ? message.address.host() : from.host();
        peer.port = message.port;
        peer.nodeId = message.nodeId;

        if (receiver_) {
            receiver_(peer);
        }
    }
}

} // namespace netproxy::node
