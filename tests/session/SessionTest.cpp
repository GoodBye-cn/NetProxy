#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "TestSupport.h"
#include "core/IoLoop.h"
#include "core/TaskLoop.h"
#include "session/DataChannel.h"
#include "session/SessionManager.h"
#include "session/SessionProtocol.h"

namespace {

using netproxy::core::IoLoop;
using netproxy::core::TaskLoop;
using netproxy::session::DataChannel;
using netproxy::session::SessionConfig;
using netproxy::session::SessionManager;
using netproxy::session::SessionMessage;
using netproxy::session::SessionType;

// 单个节点的会话运行环境，对端地址由固定 resolver 提供。
struct SessionRuntime final {
    std::uint16_t port = 0;
    IoLoop io;
    TaskLoop business;
    DataChannel channel;
    SessionManager sessions;

    SessionRuntime(std::uint16_t listenPort, std::uint32_t localNodeId, std::uint32_t peerNodeId,
                   std::uint16_t peerPort, const SessionConfig& config)
        : port(listenPort), channel(io), sessions(
                                             channel,
                                             [peerNodeId, peerPort](std::uint32_t nodeId, netproxy::PeerInfo& peer) {
                                                 if (nodeId != peerNodeId) {
                                                     return false;
                                                 }
                                                 peer.address = "127.0.0.1";
                                                 peer.port = peerPort;
                                                 peer.nodeId = peerNodeId;
                                                 return true;
                                             },
                                             localNodeId, config, business) {}

    SessionRuntime(const SessionRuntime&) = delete;
    SessionRuntime& operator=(const SessionRuntime&) = delete;

    bool start() {
        return channel.open("127.0.0.1", port) && sessions.start() && io.start() && business.start();
    }

    void stop() {
        business.stop();
        io.stop();
        sessions.stop();
        channel.close();
    }
};

void sessionProtocolRoundTrip() {
    SessionMessage open;
    open.type = SessionType::Open;
    open.sessionId = 0x12345678;
    open.nodeId = 0x0000000A;
    open.listenPort = 7001;
    open.nonce = 0xDEADBEEF;

    const std::string encoded = open.encode();
    test::checkEquals(static_cast<int>(encoded.size()), 24, "会话协议: Open 长度");

    SessionMessage decoded;
    test::checkTrue(SessionMessage::decode(encoded.data(), encoded.size(), decoded), "会话协议: Open 解码");
    test::checkTrue(decoded.type == SessionType::Open, "会话协议: Open 类型");
    test::checkEquals(static_cast<int>(decoded.sessionId), 0x12345678, "会话协议: sessionId");
    test::checkEquals(static_cast<int>(decoded.nodeId), 0x0000000A, "会话协议: nodeId");
    test::checkEquals(static_cast<int>(decoded.listenPort), 7001, "会话协议: 端口");
    test::checkEquals(static_cast<int>(decoded.nonce), 0xDEADBEEF, "会话协议: nonce");

    SessionMessage accept;
    accept.type = SessionType::Accept;
    accept.sessionId = 0x12345678;
    accept.nodeId = 0x0000000B;
    accept.nonce = 0xDEADBEEF;

    const std::string acceptBytes = accept.encode();
    test::checkEquals(static_cast<int>(acceptBytes.size()), 20, "会话协议: Accept 长度");
    test::checkTrue(SessionMessage::decode(acceptBytes.data(), acceptBytes.size(), decoded), "会话协议: Accept 解码");
    test::checkEquals(static_cast<int>(decoded.nodeId), 0x0000000B, "会话协议: Accept nodeId");

    SessionMessage close;
    close.type = SessionType::Close;
    close.nodeId = 0x0000000A;
    close.sessionId = 0x12345678;
    close.reason = 1;

    const std::string closeBytes = close.encode();
    test::checkEquals(static_cast<int>(closeBytes.size()), 20, "会话协议: Close 长度");
    test::checkTrue(SessionMessage::decode(closeBytes.data(), closeBytes.size(), decoded), "会话协议: Close 解码");
    test::checkEquals(static_cast<int>(decoded.reason), 1, "会话协议: Close 原因");
}

void sessionInvalidMessages() {
    SessionMessage message;
    test::checkFalse(SessionMessage::decode(nullptr, 0, message), "会话协议: 空报文");

    SessionMessage open;
    open.type = SessionType::Open;
    open.sessionId = 1;
    open.nodeId = 1;
    open.listenPort = 1;
    const std::string valid = open.encode();

    test::checkFalse(SessionMessage::decode(valid.data(), valid.size() - 1, message), "会话协议: 长度不足");

    std::string broken = valid;
    broken[0] = '\0';
    test::checkFalse(SessionMessage::decode(broken.data(), broken.size(), message), "会话协议: magic 错误");

    broken = valid;
    broken[12] = '\0';
    broken[13] = '\0';
    broken[14] = '\0';
    broken[15] = '\0';
    test::checkFalse(SessionMessage::decode(broken.data(), broken.size(), message), "会话协议: sessionId 为 0");

    broken = valid;
    broken[8] = '\0';
    broken[9] = '\0';
    broken[10] = '\0';
    broken[11] = '\0';
    test::checkFalse(SessionMessage::decode(broken.data(), broken.size(), message), "会话协议: nodeId 为 0");
}

void establishSession() {
    SessionConfig config;
    config.openRetryIntervalMs = 100;

    SessionRuntime runtimeA(9301, 0x11111111, 0x22222222, 9302, config);
    SessionRuntime runtimeB(9302, 0x22222222, 0x11111111, 9301, config);

    std::atomic<int> openedA{0};
    std::atomic<int> openedB{0};
    runtimeA.sessions.setHandler([&openedA](std::uint32_t, std::uint32_t, SessionManager::Event event) {
        if (event == SessionManager::Event::Opened) {
            openedA.fetch_add(1);
        }
    });
    runtimeB.sessions.setHandler([&openedB](std::uint32_t, std::uint32_t, SessionManager::Event event) {
        if (event == SessionManager::Event::Opened) {
            openedB.fetch_add(1);
        }
    });

    test::checkTrue(runtimeA.start(), "会话: A 启动");
    test::checkTrue(runtimeB.start(), "会话: B 启动");

    const std::uint32_t sessionId = runtimeA.sessions.open(0x22222222);
    test::checkTrue(sessionId != 0, "会话: 发起成功");

    bool established = false;
    for (int attempt = 0; attempt < 20 && !established; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        established = runtimeA.sessions.isEstablished(sessionId) && openedB.load() > 0;
    }
    test::checkTrue(established, "会话: 双方进入 Established");
    test::checkTrue(openedA.load() > 0, "会话: 发起方收到 Opened");

    runtimeA.stop();
    runtimeB.stop();
}

void multipleSessions() {
    SessionConfig config;
    config.openRetryIntervalMs = 100;
    config.maxSessionsPerPeer = 4;

    SessionRuntime runtimeA(9311, 0x33333333, 0x44444444, 9312, config);
    SessionRuntime runtimeB(9312, 0x44444444, 0x33333333, 9311, config);

    test::checkTrue(runtimeA.start(), "多会话: A 启动");
    test::checkTrue(runtimeB.start(), "多会话: B 启动");

    const std::uint32_t first = runtimeA.sessions.open(0x44444444);
    const std::uint32_t second = runtimeA.sessions.open(0x44444444);
    const std::uint32_t third = runtimeA.sessions.open(0x44444444);

    test::checkTrue(first != 0 && second != 0 && third != 0, "多会话: 三次发起成功");
    test::checkTrue(first != second && second != third && first != third, "多会话: 标识互不相同");

    bool established = false;
    for (int attempt = 0; attempt < 20 && !established; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        established = runtimeA.sessions.isEstablished(first) && runtimeA.sessions.isEstablished(second) &&
                      runtimeA.sessions.isEstablished(third);
    }
    test::checkTrue(established, "多会话: 全部建立");

    runtimeA.stop();
    runtimeB.stop();
}

void closeSession() {
    SessionConfig config;
    config.openRetryIntervalMs = 100;

    SessionRuntime runtimeA(9321, 0x55555555, 0x66666666, 9322, config);
    SessionRuntime runtimeB(9322, 0x66666666, 0x55555555, 9321, config);

    std::atomic<int> closedB{0};
    runtimeB.sessions.setHandler([&closedB](std::uint32_t, std::uint32_t, SessionManager::Event event) {
        if (event == SessionManager::Event::Closed) {
            closedB.fetch_add(1);
        }
    });

    test::checkTrue(runtimeA.start(), "关闭: A 启动");
    test::checkTrue(runtimeB.start(), "关闭: B 启动");

    const std::uint32_t sessionId = runtimeA.sessions.open(0x66666666);
    test::checkTrue(sessionId != 0, "关闭: 发起成功");

    bool established = false;
    for (int attempt = 0; attempt < 20 && !established; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        established = runtimeA.sessions.isEstablished(sessionId);
    }
    test::checkTrue(established, "关闭: 建立完成");

    test::checkTrue(runtimeA.sessions.close(sessionId), "关闭: 本地关闭成功");

    bool closed = false;
    for (int attempt = 0; attempt < 20 && !closed; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        closed = closedB.load() > 0;
    }
    test::checkTrue(closed, "关闭: 对端收到 Closed");
    test::checkFalse(runtimeA.sessions.isEstablished(sessionId), "关闭: 本地会话已移除");

    runtimeA.stop();
    runtimeB.stop();
}

void keepaliveKeepsSessionAlive() {
    SessionConfig config;
    config.openRetryIntervalMs = 50;
    config.keepaliveIntervalMs = 60;
    config.keepaliveTimeoutMs = 60;
    config.keepaliveRetryCount = 3;
    config.keepaliveTickMs = 10;

    SessionRuntime runtimeA(9351, 0xABAB1111, 0xCDCD2222, 9352, config);
    SessionRuntime runtimeB(9352, 0xCDCD2222, 0xABAB1111, 9351, config);

    test::checkTrue(runtimeA.start(), "保活: A 启动");
    test::checkTrue(runtimeB.start(), "保活: B 启动");

    const std::uint32_t sessionId = runtimeA.sessions.open(0xCDCD2222);
    test::checkTrue(sessionId != 0, "保活: 发起成功");

    bool established = false;
    for (int attempt = 0; attempt < 20 && !established; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        established = runtimeA.sessions.isEstablished(sessionId);
    }
    test::checkTrue(established, "保活: 建立完成");

    // 空闲超过多个保活周期，双方互发 Ping 应继续保持
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    test::checkTrue(runtimeA.sessions.isEstablished(sessionId), "保活: 空闲后仍保持");
    test::checkTrue(runtimeB.sessions.isEstablished(sessionId), "保活: 对端也保持");

    runtimeA.stop();
    runtimeB.stop();
}

void keepaliveTimeoutClosed() {
    SessionConfig config;
    config.openRetryIntervalMs = 50;
    config.keepaliveIntervalMs = 50;
    config.keepaliveTimeoutMs = 50;
    config.keepaliveRetryCount = 1;
    config.keepaliveTickMs = 10;

    SessionRuntime runtimeA(9361, 0xEFEF1111, 0x12122222, 9362, config);
    SessionRuntime runtimeB(9362, 0x12122222, 0xEFEF1111, 9361, config);

    std::atomic<int> closedA{0};
    runtimeA.sessions.setHandler([&closedA](std::uint32_t, std::uint32_t, SessionManager::Event event) {
        if (event == SessionManager::Event::Closed) {
            closedA.fetch_add(1);
        }
    });

    test::checkTrue(runtimeA.start(), "保活超时: A 启动");
    test::checkTrue(runtimeB.start(), "保活超时: B 启动");

    const std::uint32_t sessionId = runtimeA.sessions.open(0x12122222);
    test::checkTrue(sessionId != 0, "保活超时: 发起成功");

    bool established = false;
    for (int attempt = 0; attempt < 20 && !established; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        established = runtimeA.sessions.isEstablished(sessionId);
    }
    test::checkTrue(established, "保活超时: 建立完成");

    // 对端停止后不再响应 Ping
    runtimeB.stop();

    bool closed = false;
    for (int attempt = 0; attempt < 40 && !closed; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        closed = closedA.load() > 0;
    }
    test::checkTrue(closed, "保活超时: 判定断链并回调 Closed");
    test::checkFalse(runtimeA.sessions.isEstablished(sessionId), "保活超时: 会话已移除");

    runtimeA.stop();
}

void unknownPeerRejected() {
    SessionConfig config;
    config.openRetryIntervalMs = 50;
    config.openRetryCount = 2;

    // resolver 只认识 0x22222222，这里向另一个节点发起
    SessionRuntime runtime(9341, 0x99999999, 0x22222222, 9342, config);

    std::atomic<int> rejected{0};
    runtime.sessions.setHandler([&rejected](std::uint32_t, std::uint32_t peerNodeId, SessionManager::Event event) {
        if (event == SessionManager::Event::Rejected && peerNodeId == 0xAAAAAAAA) {
            rejected.fetch_add(1);
        }
    });

    test::checkTrue(runtime.start(), "未知节点: 启动");

    const std::uint32_t sessionId = runtime.sessions.open(0xAAAAAAAA);
    test::checkTrue(sessionId != 0, "未知节点: 仍返回会话标识");

    bool done = false;
    for (int attempt = 0; attempt < 20 && !done; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        done = rejected.load() > 0;
    }
    test::checkTrue(done, "未知节点: 回调 Rejected");
    test::checkFalse(runtime.sessions.isEstablished(sessionId), "未知节点: 不会建立");

    runtime.stop();
}

void handshakeTimeout() {
    SessionConfig config;
    config.openRetryIntervalMs = 50;
    config.openRetryCount = 2;

    // 对端端口没有监听，Open 不会有响应
    SessionRuntime runtimeA(9331, 0x77777777, 0x88888888, 9332, config);

    std::atomic<int> rejected{0};
    runtimeA.sessions.setHandler([&rejected](std::uint32_t, std::uint32_t, SessionManager::Event event) {
        if (event == SessionManager::Event::Rejected) {
            rejected.fetch_add(1);
        }
    });

    test::checkTrue(runtimeA.start(), "超时: A 启动");

    const std::uint32_t sessionId = runtimeA.sessions.open(0x88888888);
    test::checkTrue(sessionId != 0, "超时: 发起成功");

    bool timedOut = false;
    for (int attempt = 0; attempt < 40 && !timedOut; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        timedOut = rejected.load() > 0;
    }
    test::checkTrue(timedOut, "超时: 重传耗尽后回调 Rejected");
    test::checkFalse(runtimeA.sessions.isEstablished(sessionId), "超时: 会话未建立");

    runtimeA.stop();
}

} // namespace

void runSessionTests() {
    NETPROXY_CASE(sessionProtocolRoundTrip);
    NETPROXY_CASE(sessionInvalidMessages);
    NETPROXY_CASE(establishSession);
    NETPROXY_CASE(multipleSessions);
    NETPROXY_CASE(closeSession);
    NETPROXY_CASE(keepaliveKeepsSessionAlive);
    NETPROXY_CASE(keepaliveTimeoutClosed);
    NETPROXY_CASE(unknownPeerRejected);
    NETPROXY_CASE(handshakeTimeout);
}
