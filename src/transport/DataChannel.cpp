#include "transport/DataChannel.h"

#include "Log.h"
#include "core/Bind.h"

#include <functional>
#include <utility>

namespace netproxy::transport {

DataChannel::DataChannel(core::IoLoop& io) : io_(io) {}

PacketSender DataChannel::sender() {
    return core::bindFront<&DataChannel::send>(this);
}

DataChannel::~DataChannel() {
    close();
}

bool DataChannel::open(const std::string& bindAddress, std::uint16_t port) {
    if (opened_) {
        return true;
    }

    const net::IPAddress address =
        bindAddress.empty() ? net::IPAddress::any(port) : net::IPAddress::parse(bindAddress, port);

    if (!address.isValid()) {
        Log::error("data channel open failed: invalid address ", bindAddress, ":", port);
        return false;
    }

    if (!socket_.open(address) || !socket_.bind(address) || !socket_.setNonBlocking(true)) {
        Log::error("data channel open failed: cannot bind ", address.toString());
        socket_.close();
        return false;
    }

    opened_ = true;
    Log::debug("data channel opened port=", socket_.localPort());
    return true;
}

void DataChannel::close() {
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

bool DataChannel::isOpen() const {
    return opened_;
}

std::uint16_t DataChannel::localPort() const {
    if (!opened_) {
        return 0;
    }
    return socket_.localPort();
}

bool DataChannel::setReceiver(Receiver receiver) {
    if (!opened_) {
        Log::warning("data channel setReceiver failed: channel not open");
        return false;
    }

    receiver_ = std::move(receiver);

    if (registered_) {
        return true;
    }

    registered_ =
        io_.dispatcher().add(socket_.nativeHandle(), net::Poller::Read, core::bindFront<&DataChannel::onEvent>(this));
    if (!registered_) {
        Log::error("data channel setReceiver failed: cannot register read event");
    }
    return registered_;
}

void DataChannel::send(const std::string& packet, const net::IPAddress& target) {
    io_.post(std::bind(&DataChannel::sendOnIoThread, this, packet, target));
}

void DataChannel::sendOnIoThread(const std::string& packet, const net::IPAddress& target) {
    if (socket_.isValid()) {
        socket_.sendTo(packet, target);
    }
}

void DataChannel::onEvent(net::SocketHandle, std::uint32_t) {
    while (true) {
        char buffer[1500] = {};
        net::IPAddress from;
        const int received = socket_.receiveFrom(buffer, sizeof(buffer), from);
        if (received <= 0) {
            break;
        }

        if (receiver_) {
            receiver_(from, buffer, static_cast<std::size_t>(received));
        }
    }
}

} // namespace netproxy::transport
