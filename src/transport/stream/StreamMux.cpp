#include "StreamMux.h"

#include <algorithm>
#include <utility>

namespace netproxy::transport {

namespace {

// 关闭原因放入 1 字节负载。
std::string reasonPayload(protocol::Reason reason) {
    return std::string(1, static_cast<char>(reason));
}

// 构造流控制帧。
protocol::PeerFrame makeStreamFrame(protocol::FrameType type, std::uint32_t streamId, const std::string& payload = {}) {
    protocol::PeerFrame frame;
    frame.type = static_cast<std::uint8_t>(type);
    frame.streamId = streamId;
    frame.payload = payload;
    return frame;
}

} // namespace

StreamMux::StreamMux(std::uint32_t peerNodeId, const StreamConfig& config, FrameSender sender)
    : peerNodeId_(peerNodeId), config_(config), sender_(std::move(sender)) {}

void StreamMux::onFrame(const protocol::PeerFrame& frame) {
    switch (static_cast<protocol::FrameType>(frame.type)) {
    case protocol::FrameType::Data:
        handleData(frame);
        break;
    case protocol::FrameType::StreamOpen:
        handleStreamOpen(frame);
        break;
    case protocol::FrameType::StreamAccept:
        handleStreamAccept(frame);
        break;
    case protocol::FrameType::StreamReject:
        handleStreamReject(frame);
        break;
    case protocol::FrameType::StreamClose:
        handleStreamClose(frame);
        break;
    case protocol::FrameType::Ping:
    case protocol::FrameType::Close:
        // 连接级控制帧由连接层处理。
        break;
    }
}

void StreamMux::onConnectionUp() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = true;
    }

    flushPending();
}

void StreamMux::onConnectionClosed(protocol::Reason reason) {
    std::vector<Emit> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;

        for (const auto& entry : streams_) {
            const bool queued = isPendingLocked(entry.first);
            events.push_back({entry.first, queued ? protocol::StreamEvent::Rejected : protocol::StreamEvent::Closed,
                              queued ? protocol::Reason::Timeout : reason});
        }

        streams_.clear();
        pending_.clear();
    }

    dispatch({}, events);
}

bool StreamMux::openStream(std::uint32_t streamId) {
    if (streamId == 0) {
        return false;
    }

    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (streams_.find(streamId) != streams_.end()) {
            return false;
        }

        if (streams_.size() >= config_.maxStreams) {
            return false;
        }

        streams_.emplace(streamId, Stream(streamId, peerNodeId_, true));
        if (!connected_) {
            pending_.push_back(streamId);
            queued = true;
        }
    }

    if (queued) {
        return true;
    }

    return sendStreamOpen(streamId);
}

bool StreamMux::sendData(std::uint32_t streamId, const void* data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return false;
    }

    protocol::PeerFrame frame;
    frame.type = static_cast<std::uint8_t>(protocol::FrameType::Data);
    frame.streamId = streamId;
    frame.payload.assign(static_cast<const char*>(data), size);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = streams_.find(streamId);
        if (entry == streams_.end() || !entry->second.isOpen()) {
            return false;
        }
    }

    // 长度与连接状态由连接层校验，这里透传结果。
    return sender_ && sender_(std::move(frame));
}

bool StreamMux::closeStream(std::uint32_t streamId) {
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = streams_.find(streamId);
        if (entry == streams_.end()) {
            return false;
        }

        queued = isPendingLocked(streamId);
        removeStreamLocked(streamId);
    }

    // 排队中的流还没发过 StreamOpen，对端不知道它，不必发 StreamClose。
    if (!queued) {
        protocol::PeerFrame frame = makeStreamFrame(protocol::FrameType::StreamClose, streamId,
                                          reasonPayload(protocol::Reason::Local));
        if (sender_) {
            sender_(std::move(frame));
        }
    }

    emitEvent(streamId, protocol::StreamEvent::Closed, protocol::Reason::Local);
    return true;
}

bool StreamMux::hasStreams() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !streams_.empty();
}

bool StreamMux::isStreamOpen(std::uint32_t streamId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto entry = streams_.find(streamId);
    return entry != streams_.end() && entry->second.isOpen();
}

bool StreamMux::ownsStream(std::uint32_t streamId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return streams_.find(streamId) != streams_.end();
}

std::vector<StreamRecord> StreamMux::streams() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<StreamRecord> result;
    result.reserve(streams_.size());
    for (const auto& entry : streams_) {
        result.push_back(
            StreamRecord{entry.first, peerNodeId_, entry.second.isOpen(), entry.second.locallyInitiated()});
    }
    return result;
}

void StreamMux::setStreamHandler(StreamHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    streamHandler_ = std::move(handler);
}

void StreamMux::setDataHandler(DataHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    dataHandler_ = std::move(handler);
}

void StreamMux::handleStreamOpen(const protocol::PeerFrame& frame) {
    std::vector<protocol::PeerFrame> frames;
    std::vector<Emit> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        const auto existing = streams_.find(frame.streamId);
        if (existing != streams_.end()) {
            // 幂等：重复的 StreamOpen 只重发 Accept，不重复回调 Opened。
            frames.push_back(makeStreamFrame(protocol::FrameType::StreamAccept, frame.streamId));
        } else if (streams_.size() >= config_.maxStreams) {
            frames.push_back(makeStreamFrame(protocol::FrameType::StreamReject, frame.streamId,
                                             reasonPayload(protocol::Reason::Error)));
        } else {
            Stream stream(frame.streamId, peerNodeId_, false);
            stream.markOpen();
            streams_.emplace(frame.streamId, std::move(stream));

            frames.push_back(makeStreamFrame(protocol::FrameType::StreamAccept, frame.streamId));
            events.push_back(
                {frame.streamId, protocol::StreamEvent::Opened, static_cast<protocol::Reason>(0)});
        }
    }

    dispatch(std::move(frames), events);
}

void StreamMux::handleStreamAccept(const protocol::PeerFrame& frame) {
    std::vector<Emit> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        const auto entry = streams_.find(frame.streamId);
        if (entry == streams_.end() || entry->second.state() != StreamState::Opening) {
            return;
        }

        entry->second.markOpen();
        events.push_back({frame.streamId, protocol::StreamEvent::Opened, static_cast<protocol::Reason>(0)});
    }

    dispatch({}, events);
}

void StreamMux::handleStreamReject(const protocol::PeerFrame& frame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (streams_.find(frame.streamId) == streams_.end()) {
            return;
        }

        removeStreamLocked(frame.streamId);
    }

    emitEvent(frame.streamId, protocol::StreamEvent::Rejected, protocol::Reason::Error);
}

void StreamMux::handleStreamClose(const protocol::PeerFrame& frame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (streams_.find(frame.streamId) == streams_.end()) {
            return;
        }

        removeStreamLocked(frame.streamId);
    }

    emitEvent(frame.streamId, protocol::StreamEvent::Closed, protocol::Reason::Remote);
}

void StreamMux::handleData(const protocol::PeerFrame& frame) {
    const std::uint32_t streamId = frame.streamId;
    const std::string payload = frame.payload;

    DataHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (streams_.find(streamId) == streams_.end()) {
            ++droppedFrames_;
            return;
        }

        // 与拆分前一致：只要求流存在，不校验 Opening/Open。
        handler = dataHandler_;
    }

    if (handler) {
        handler(streamId, peerNodeId_, payload.data(), payload.size());
    }
}

bool StreamMux::sendStreamOpen(std::uint32_t streamId) {
    protocol::PeerFrame frame = makeStreamFrame(protocol::FrameType::StreamOpen, streamId);
    if (sender_ && sender_(std::move(frame))) {
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        removeStreamLocked(streamId);
    }

    emitEvent(streamId, protocol::StreamEvent::Rejected, protocol::Reason::Error);
    return false;
}

void StreamMux::flushPending() {
    std::vector<std::uint32_t> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.assign(pending_.begin(), pending_.end());
        pending_.clear();
    }

    for (const std::uint32_t streamId : pending) {
        sendStreamOpen(streamId);
    }
}

void StreamMux::removeStreamLocked(std::uint32_t streamId) {
    streams_.erase(streamId);
    const auto entry = std::find(pending_.begin(), pending_.end(), streamId);
    if (entry != pending_.end()) {
        pending_.erase(entry);
    }
}

bool StreamMux::isPendingLocked(std::uint32_t streamId) const {
    return std::find(pending_.begin(), pending_.end(), streamId) != pending_.end();
}

void StreamMux::dispatch(std::vector<protocol::PeerFrame> frames, const std::vector<Emit>& events) {
    for (protocol::PeerFrame& frame : frames) {
        if (sender_) {
            sender_(std::move(frame));
        }
    }

    if (events.empty()) {
        return;
    }

    StreamHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = streamHandler_;
    }

    if (!handler) {
        return;
    }

    for (const Emit& event : events) {
        handler(event.streamId, peerNodeId_, event.event, event.reason);
    }
}

void StreamMux::emitEvent(std::uint32_t streamId, protocol::StreamEvent event, protocol::Reason reason) {
    dispatch({}, {Emit{streamId, event, reason}});
}

} // namespace netproxy::transport
