#include "DataChannel.h"

#include "Log.h"

#include <utility>

namespace netproxy::session {

DataChannel::DataChannel(core::IoLoop& io) : io_(io) {}

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
        Log::error("数据通道打开失败: 地址无效 ", bindAddress, ":", port);
        return false;
    }

    if (!socket_.open(address) || !socket_.bind(address) || !socket_.setNonBlocking(true)) {
        Log::error("数据通道打开失败: 无法绑定 ", address.toString());
        socket_.close();
        return false;
    }

    opened_ = true;
    Log::debug("数据通道已打开 port=", socket_.localPort());
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
        Log::warning("数据通道注册回调失败: 通道未打开");
        return false;
    }

    receiver_ = std::move(receiver);

    if (registered_) {
        return true;
    }

    registered_ = io_.dispatcher().add(socket_.nativeHandle(), net::Poller::Read,
                                       [this](net::SocketHandle, std::uint32_t) { handleReadable(); });
    if (!registered_) {
        Log::error("数据通道注册回调失败: 无法注册读事件");
    }
    return registered_;
}

void DataChannel::send(const std::string& packet, const net::IPAddress& target) {
    io_.post([this, packet, target] {
        if (socket_.isValid()) {
            socket_.sendTo(packet, target);
        }
    });
}

void DataChannel::handleReadable() {
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

} // namespace netproxy::session
