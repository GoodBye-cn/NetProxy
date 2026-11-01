#include "Stream.h"

#include <utility>

namespace netproxy::transport {

Stream::Stream(std::uint32_t streamId, std::uint32_t peerNodeId, bool locallyInitiated)
    : streamId_(streamId), peerNodeId_(peerNodeId), locallyInitiated_(locallyInitiated) {}

std::uint32_t Stream::id() const {
    return streamId_;
}

std::uint32_t Stream::peerNodeId() const {
    return peerNodeId_;
}

StreamState Stream::state() const {
    return state_;
}

bool Stream::isOpen() const {
    return state_ == StreamState::Open;
}

bool Stream::locallyInitiated() const {
    return locallyInitiated_;
}

void Stream::setEventCallback(EventCallback handler) {
    eventHandler_ = std::move(handler);
}

void Stream::setDataCallback(DataCallback handler) {
    dataHandler_ = std::move(handler);
}

bool Stream::markOpen() {
    if (state_ != StreamState::Opening) {
        return false;
    }

    state_ = StreamState::Open;
    return true;
}

void Stream::deliverData(const char* data, std::size_t size) {
    if (state_ == StreamState::Closed || dataHandler_ == nullptr) {
        return;
    }

    dataHandler_(streamId_, data, size);
}

void Stream::close(protocol::StreamEvent event, protocol::Reason reason) {
    if (state_ == StreamState::Closed) {
        return;
    }

    state_ = StreamState::Closed;
    if (eventHandler_) {
        eventHandler_(streamId_, event, reason);
    }
}

} // namespace netproxy::transport
