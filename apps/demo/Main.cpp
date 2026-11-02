#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "Log.h"
#include "P2PService.h"

namespace {

std::atomic<bool> running{true};

void handleSignal(int) {
    running = false;
}

void printUsage(const char* program) {
    netproxy::Log::warning("usage: ", program, " [options]");
    netproxy::Log::warning("  --node-id <N>       node id, 0 means random (default 0)");
    netproxy::Log::warning("  --port <N>          data port, 0 means system assigned (default 0)");
    netproxy::Log::warning("  --bind <IP>         data channel bind address, empty means 0.0.0.0");
    netproxy::Log::warning("  --group <IP>        multicast group, default 239.255.42.99");
    netproxy::Log::warning("  --group-port <N>    multicast port, default 45454");
    netproxy::Log::warning("  --announce-ms <N>   announce interval, default 3000");
    netproxy::Log::warning("  --timeout-ms <N>    peer timeout, default 10000");
    netproxy::Log::warning("  --open <N>          open a stream to the given peer after start");
}

void printCommands() {
    netproxy::Log::warning(
        "commands: open <nodeId> | close <streamId> | send <streamId> <text> | streams | peers | quit");
}

bool parseUInt(const char* text, unsigned long& value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }

    value = parsed;
    return true;
}

// 处理一行交互命令。
void handleCommand(netproxy::P2PService& service, const std::string& line) {
    std::istringstream stream(line);
    std::string command;
    stream >> command;

    if (command.empty()) {
        return;
    }

    if (command == "open") {
        unsigned long nodeId = 0;
        if (!(stream >> nodeId)) {
            printCommands();
            return;
        }

        const std::uint32_t streamId = service.openStream(static_cast<std::uint32_t>(nodeId));
        if (streamId == 0) {
            netproxy::Log::warning("open stream failed nodeId=", nodeId);
        } else {
            netproxy::Log::info("opening stream streamId=", streamId, " peer=", nodeId);
        }
        return;
    }

    if (command == "close") {
        unsigned long streamId = 0;
        if (!(stream >> streamId)) {
            printCommands();
            return;
        }

        if (service.closeStream(static_cast<std::uint32_t>(streamId))) {
            netproxy::Log::info("stream closed streamId=", streamId);
        } else {
            netproxy::Log::warning("close stream failed streamId=", streamId);
        }
        return;
    }

    if (command == "send") {
        unsigned long streamId = 0;
        if (!(stream >> streamId)) {
            printCommands();
            return;
        }

        std::string text;
        std::getline(stream, text);
        if (!text.empty() && text.front() == ' ') {
            text.erase(0, 1);
        }

        if (!service.isStreamOpen(static_cast<std::uint32_t>(streamId))) {
            netproxy::Log::warning("stream not open yet, wait for the stream opened log before sending streamId=",
                                   streamId);
            return;
        }

        if (service.sendData(static_cast<std::uint32_t>(streamId), text.data(), text.size())) {
            netproxy::Log::info("data submitted streamId=", streamId, " size=", text.size());
        } else {
            netproxy::Log::warning("submit data failed streamId=", streamId);
        }
        return;
    }

    if (command == "streams") {
        const std::vector<netproxy::StreamInfo> streams = service.streams();
        if (streams.empty()) {
            netproxy::Log::info("no streams");
            return;
        }

        for (const netproxy::StreamInfo& info : streams) {
            netproxy::Log::info("stream streamId=", info.streamId, " peer=", info.peerNodeId,
                                info.open ? " established" : " handshaking");
        }
        return;
    }

    if (command == "peers") {
        const std::vector<netproxy::PeerInfo> peers = service.peers();
        if (peers.empty()) {
            netproxy::Log::info("no peers discovered");
            return;
        }

        for (const netproxy::PeerInfo& peer : peers) {
            netproxy::Log::info("peer nodeId=", peer.nodeId, " ", peer.address, ":", peer.port);
        }
        return;
    }

    if (command == "quit" || command == "exit") {
        running = false;
        return;
    }

    if (command == "help") {
        printCommands();
        return;
    }

    netproxy::Log::warning("unknown command ", command);
    printCommands();
}

} // namespace

int main(int argc, char* argv[]) {
    // 源码中的文案是 UTF-8（/utf-8），与控制台默认代码页（如 936）不一致会显示为乱码。
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    netproxy::P2PServiceConfig config;
    std::uint32_t openTarget = 0;
    std::atomic<std::uint32_t> streamId{0};

    auto readValue = [&argc, &argv](int& index, unsigned long& value) -> bool {
        if (index + 1 >= argc) {
            return false;
        }
        return parseUInt(argv[++index], value);
    };

    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        unsigned long value = 0;

        if (option == "--help" || option == "-h") {
            printUsage(argv[0]);
            return 0;
        }

        if (option == "--node-id") {
            if (!readValue(index, value) || value > 0xFFFFFFFFUL) {
                printUsage(argv[0]);
                return 1;
            }
            config.nodeId = static_cast<std::uint32_t>(value);
        } else if (option == "--open") {
            if (!readValue(index, value) || value == 0 || value > 0xFFFFFFFFUL) {
                printUsage(argv[0]);
                return 1;
            }
            openTarget = static_cast<std::uint32_t>(value);
        } else if (option == "--port") {
            if (!readValue(index, value) || value > 0xFFFFUL) {
                printUsage(argv[0]);
                return 1;
            }
            config.dataPort = static_cast<std::uint16_t>(value);
        } else if (option == "--group-port") {
            if (!readValue(index, value) || value > 0xFFFFUL) {
                printUsage(argv[0]);
                return 1;
            }
            config.multicastPort = static_cast<std::uint16_t>(value);
        } else if (option == "--announce-ms") {
            if (!readValue(index, value) || value == 0 || value > 3600000UL) {
                printUsage(argv[0]);
                return 1;
            }
            config.announceIntervalMs = static_cast<int>(value);
        } else if (option == "--timeout-ms") {
            if (!readValue(index, value) || value == 0 || value > 3600000UL) {
                printUsage(argv[0]);
                return 1;
            }
            config.peerTimeoutMs = static_cast<int>(value);
        } else if (option == "--bind" || option == "--group") {
            if (index + 1 >= argc) {
                printUsage(argv[0]);
                return 1;
            }
            const char* argument = argv[++index];
            if (option == "--bind") {
                config.dataBindAddress = argument;
            } else {
                config.multicastGroup = argument;
            }
        } else {
            netproxy::Log::error("unknown option ", option);
            printUsage(argv[0]);
            return 1;
        }
    }

    netproxy::P2PService service;
    service.setPeerHandler([](const netproxy::PeerInfo& peer) {
        netproxy::Log::info("peer discovered nodeId=", peer.nodeId, " ", peer.address, ":", peer.port);
    });
    service.setDataHandler([](std::uint32_t id, std::uint32_t peerNodeId, const char* data, std::size_t size) {
        netproxy::Log::info("data received streamId=", id, " peer=", peerNodeId, " size=", size,
                            " content=", std::string(data, size));
    });
    service.setStreamHandler([&openTarget, &streamId](std::uint32_t id, std::uint32_t peerNodeId,
                                                      netproxy::StreamEvent event, std::uint8_t reason) {
        switch (event) {
        case netproxy::StreamEvent::Opened:
            netproxy::Log::info("stream opened streamId=", id, " peer=", peerNodeId);
            break;
        case netproxy::StreamEvent::Rejected:
            netproxy::Log::warning("stream rejected or timed out streamId=", id, " peer=", peerNodeId,
                                   " reason=", static_cast<int>(reason));
            if (peerNodeId == openTarget) {
                streamId = 0; // 对端未发现或拒绝，允许自动重试
            }
            break;
        case netproxy::StreamEvent::Closed:
            netproxy::Log::info("stream closed streamId=", id, " peer=", peerNodeId,
                                " reason=", static_cast<int>(reason));
            break;
        }
    });

    if (!service.start(config)) {
        netproxy::Log::error("service start failed");
        return 1;
    }

    netproxy::Log::info("service started nodeId=", service.nodeId(), " dataPort=", service.dataPort());
    netproxy::Log::info("discovery channel ", config.multicastGroup, ":", config.multicastPort);
    printCommands();

    // 交互线程：读取标准输入，退出命令或 EOF 时结束运行
    std::thread input([&service] {
        std::string line;
        while (running.load() && std::getline(std::cin, line)) {
            handleCommand(service, line);
        }
        running = false;
    });
    input.detach();

    auto nextAttempt = std::chrono::steady_clock::now();

    while (running.load()) {
        if (openTarget != 0 && streamId.load() == 0 && std::chrono::steady_clock::now() >= nextAttempt) {
            const std::uint32_t opened = service.openStream(openTarget);
            if (opened != 0) {
                streamId = opened;
                netproxy::Log::info("opening stream streamId=", opened, " peer=", openTarget);
            } else {
                netproxy::Log::info("open stream not accepted, retry later nodeId=", openTarget);
            }
            nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    service.stop();
    return 0;
}
