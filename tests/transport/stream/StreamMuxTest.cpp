#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "TestSupport.h"
#include "transport/stream/StreamMux.h"

namespace {

using netproxy::protocol::FrameType;
using netproxy::protocol::CloseReason;
using netproxy::protocol::StreamEvent;
using netproxy::protocol::PeerFrame;
using netproxy::transport::StreamConfig;
using netproxy::transport::StreamMux;
using netproxy::transport::StreamRecord;

// 捕获流层发出的帧。
struct Capture {
    std::vector<PeerFrame> frames;

    StreamMux::FrameSender sender() {
        return [this](PeerFrame frame) {
            frames.push_back(std::move(frame));
            return true;
        };
    }

    std::size_t count(FrameType type) const {
        std::size_t total = 0;
        for (const PeerFrame& frame : frames) {
            if (frame.type == static_cast<std::uint8_t>(type)) {
                ++total;
            }
        }
        return total;
    }

    const PeerFrame* last() const {
        return frames.empty() ? nullptr : &frames.back();
    }
};

StreamConfig smallConfig(std::size_t maxStreams) {
    StreamConfig config;
    config.maxStreams = maxStreams;
    return config;
}

PeerFrame streamFrame(FrameType type, std::uint32_t streamId) {
    PeerFrame frame;
    frame.type = static_cast<std::uint8_t>(type);
    frame.streamId = streamId;
    return frame;
}

PeerFrame dataFrame(std::uint32_t streamId, const std::string& payload) {
    PeerFrame frame = streamFrame(FrameType::Data, streamId);
    frame.payload = payload;
    return frame;
}

// 记录流事件的辅助。
struct Events {
    std::vector<StreamEvent> types;
    std::vector<CloseReason> reasons;
    std::uint32_t lastStreamId = 0;

    StreamMux::StreamHandler handler() {
        return [this](std::uint32_t streamId, std::uint32_t, StreamEvent event, CloseReason reason) {
            lastStreamId = streamId;
            types.push_back(event);
            reasons.push_back(reason);
        };
    }
};

void openStreamQueuesUntilConnected() {
    Capture capture;
    StreamMux mux(0x22, smallConfig(4), capture.sender());

    test::checkTrue(mux.openStream(1), "排队: 开流已受理");
    test::checkEquals(static_cast<int>(capture.frames.size()), 0, "排队: 未连接时不发帧");
    test::checkTrue(mux.ownsStream(1), "排队: 流已建表");
    test::checkFalse(mux.isStreamOpen(1), "排队: 仍未打开");

    mux.onConnectionUp();
    test::checkEquals(static_cast<int>(capture.frames.size()), 1, "排队: 连接就绪后补发 StreamOpen");

    const PeerFrame* frame = capture.last();
    test::checkTrue(frame != nullptr && frame->type == static_cast<std::uint8_t>(FrameType::StreamOpen),
                    "排队: 补发帧类型正确");
    test::checkTrue(frame != nullptr && frame->streamId == 1, "排队: 补发帧流号正确");
}

void acceptOpensStream() {
    Capture capture;
    Events events;
    StreamMux mux(0x22, smallConfig(4), capture.sender());
    mux.setStreamHandler(events.handler());

    mux.openStream(7);
    mux.onConnectionUp();
    mux.onFrame(streamFrame(FrameType::StreamAccept, 7));

    test::checkTrue(mux.isStreamOpen(7), "接受: 流已打开");
    test::checkEquals(static_cast<int>(events.types.size()), 1, "接受: 回调一次");
    test::checkTrue(!events.types.empty() && events.types[0] == StreamEvent::Opened, "接受: 事件为 Opened");
    test::checkTrue(events.lastStreamId == 7, "接受: 流号正确");

    const std::vector<StreamRecord> records = mux.streams();
    test::checkEquals(static_cast<int>(records.size()), 1, "接受: 流表一条");
    test::checkTrue(!records.empty() && records[0].locallyInitiated, "接受: 标记为本地发起");
    test::checkTrue(!records.empty() && records[0].peerNodeId == 0x22, "接受: 对端标识正确");
}

void remoteOpenCreatesStream() {
    Capture capture;
    Events events;
    StreamMux mux(0x22, smallConfig(4), capture.sender());
    mux.setStreamHandler(events.handler());

    mux.onFrame(streamFrame(FrameType::StreamOpen, 9));

    test::checkTrue(mux.isStreamOpen(9), "对端开流: 已打开");
    test::checkEquals(static_cast<int>(capture.count(FrameType::StreamAccept)), 1, "对端开流: 回 Accept");
    test::checkEquals(static_cast<int>(events.types.size()), 1, "对端开流: 回调一次");
    test::checkTrue(!events.types.empty() && events.types[0] == StreamEvent::Opened, "对端开流: 事件为 Opened");

    const std::vector<StreamRecord> records = mux.streams();
    test::checkTrue(!records.empty() && !records[0].locallyInitiated, "对端开流: 标记为非本地发起");
}

void duplicateOpenIsIdempotent() {
    Capture capture;
    Events events;
    StreamMux mux(0x22, smallConfig(4), capture.sender());
    mux.setStreamHandler(events.handler());

    mux.onFrame(streamFrame(FrameType::StreamOpen, 9));
    mux.onFrame(streamFrame(FrameType::StreamOpen, 9));

    test::checkEquals(static_cast<int>(capture.count(FrameType::StreamAccept)), 2, "幂等: 重复回 Accept");
    test::checkEquals(static_cast<int>(events.types.size()), 1, "幂等: 不重复回调 Opened");
    test::checkEquals(static_cast<int>(mux.streams().size()), 1, "幂等: 流表仍一条");
}

void maxStreamsRejects() {
    Capture capture;
    Events events;
    StreamMux mux(0x22, smallConfig(1), capture.sender());
    mux.setStreamHandler(events.handler());

    mux.onFrame(streamFrame(FrameType::StreamOpen, 1));
    mux.onFrame(streamFrame(FrameType::StreamOpen, 2));

    test::checkEquals(static_cast<int>(mux.streams().size()), 1, "上限: 只保留一条流");
    test::checkEquals(static_cast<int>(capture.count(FrameType::StreamReject)), 1, "上限: 回 StreamReject");
    test::checkTrue(!events.types.empty() && events.types.back() == StreamEvent::Opened, "上限: 被拒流不回调 Opened");
}

void rejectRemovesStream() {
    Capture capture;
    Events events;
    StreamMux mux(0x22, smallConfig(4), capture.sender());
    mux.setStreamHandler(events.handler());

    mux.openStream(3);
    mux.onConnectionUp();
    mux.onFrame(streamFrame(FrameType::StreamReject, 3));

    test::checkFalse(mux.ownsStream(3), "拒绝: 流已删除");
    test::checkTrue(!events.types.empty() && events.types.back() == StreamEvent::Rejected, "拒绝: 事件为 Rejected");
}

void remoteCloseRemovesStream() {
    Capture capture;
    Events events;
    StreamMux mux(0x22, smallConfig(4), capture.sender());
    mux.setStreamHandler(events.handler());

    mux.onFrame(streamFrame(FrameType::StreamOpen, 4));
    mux.onFrame(streamFrame(FrameType::StreamClose, 4));

    test::checkFalse(mux.ownsStream(4), "对端关闭: 流已删除");
    test::checkTrue(!events.types.empty() && events.types.back() == StreamEvent::Closed, "对端关闭: 事件为 Closed");
    test::checkTrue(!events.reasons.empty() && events.reasons.back() == CloseReason::Remote, "对端关闭: 原因为 Remote");
}

void localCloseSendsFrame() {
    Capture capture;
    Events events;
    StreamMux mux(0x22, smallConfig(4), capture.sender());
    mux.setStreamHandler(events.handler());

    mux.openStream(5);
    mux.onConnectionUp();
    mux.onFrame(streamFrame(FrameType::StreamAccept, 5));

    test::checkTrue(mux.closeStream(5), "本地关闭: 成功");
    test::checkEquals(static_cast<int>(capture.count(FrameType::StreamClose)), 1, "本地关闭: 发出 StreamClose");
    test::checkTrue(!events.types.empty() && events.reasons.back() == CloseReason::Local, "本地关闭: 原因为 Local");
    test::checkFalse(mux.closeStream(5), "本地关闭: 重复关闭失败");
}

void pendingStreamRejectedOnClose() {
    Capture capture;
    Events events;
    StreamMux mux(0x22, smallConfig(4), capture.sender());
    mux.setStreamHandler(events.handler());

    mux.openStream(6);
    mux.onConnectionClosed(CloseReason::Timeout);

    test::checkFalse(mux.ownsStream(6), "断连: 排队流被清理");
    test::checkTrue(!events.types.empty() && events.types.back() == StreamEvent::Rejected, "断连: 排队流为 Rejected");
    test::checkTrue(!events.reasons.empty() && events.reasons.back() == CloseReason::Timeout, "断连: 排队流原因为 Timeout");
}

void openStreamClosedOnConnectionLoss() {
    Capture capture;
    Events events;
    StreamMux mux(0x22, smallConfig(4), capture.sender());
    mux.setStreamHandler(events.handler());

    mux.openStream(7);
    mux.onConnectionUp();
    mux.onFrame(streamFrame(FrameType::StreamAccept, 7));

    mux.onConnectionClosed(CloseReason::Remote);

    test::checkTrue(!events.types.empty() && events.types.back() == StreamEvent::Closed, "断连: 已开流为 Closed");
    test::checkTrue(!events.reasons.empty() && events.reasons.back() == CloseReason::Remote, "断连: 已开流原因透传");
    test::checkEquals(static_cast<int>(mux.streams().size()), 0, "断连: 流表清空");
    test::checkFalse(mux.hasStreams(), "断连: 无活跃流");
}

void sendDataRequiresOpenState() {
    Capture capture;
    StreamMux mux(0x22, smallConfig(4), capture.sender());

    mux.openStream(8);
    mux.onConnectionUp();
    test::checkFalse(mux.sendData(8, "x", 1), "发送: Opening 状态拒绝");

    mux.onFrame(streamFrame(FrameType::StreamAccept, 8));
    test::checkTrue(mux.sendData(8, "x", 1), "发送: Open 状态受理");
    test::checkEquals(static_cast<int>(capture.count(FrameType::Data)), 1, "发送: 发出 Data 帧");
}

void dataDeliveryReentersSendData() {
    Capture capture;
    StreamMux mux(0x22, smallConfig(4), capture.sender());

    mux.onFrame(streamFrame(FrameType::StreamOpen, 10));

    // 应用在 DataHandler 内立即回发：Mux 持锁回调会在这里自锁。
    mux.setDataHandler([&mux](std::uint32_t streamId, std::uint32_t, const char*, std::size_t) {
        mux.sendData(streamId, "pong", 4);
    });

    mux.onFrame(dataFrame(10, "ping"));

    test::checkEquals(static_cast<int>(capture.count(FrameType::Data)), 1, "重入: 回调内回发成功");
}

void unknownStreamDataDropped() {
    Capture capture;
    StreamMux mux(0x22, smallConfig(4), capture.sender());

    mux.setDataHandler([](std::uint32_t, std::uint32_t, const char*, std::size_t) { test::checkTrue(false, "未知流不应交付"); });
    mux.onFrame(dataFrame(404, "x"));

    test::checkEquals(static_cast<int>(mux.streams().size()), 0, "未知流: 不建表");
}

} // namespace

void runStreamMuxTests() {
    NETPROXY_CASE(openStreamQueuesUntilConnected);
    NETPROXY_CASE(acceptOpensStream);
    NETPROXY_CASE(remoteOpenCreatesStream);
    NETPROXY_CASE(duplicateOpenIsIdempotent);
    NETPROXY_CASE(maxStreamsRejects);
    NETPROXY_CASE(rejectRemovesStream);
    NETPROXY_CASE(remoteCloseRemovesStream);
    NETPROXY_CASE(localCloseSendsFrame);
    NETPROXY_CASE(pendingStreamRejectedOnClose);
    NETPROXY_CASE(openStreamClosedOnConnectionLoss);
    NETPROXY_CASE(sendDataRequiresOpenState);
    NETPROXY_CASE(dataDeliveryReentersSendData);
    NETPROXY_CASE(unknownStreamDataDropped);
}
