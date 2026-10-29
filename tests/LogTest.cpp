#include <functional>
#include <iostream>
#include <sstream>
#include <string>

#include "Log.h"
#include "TestSupport.h"

namespace {

std::string captureStdout(const std::function<void()>& action) {
    std::ostringstream captured;
    std::streambuf* original = std::cout.rdbuf(captured.rdbuf());
    action();
    std::cout.rdbuf(original);
    return captured.str();
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void defaultHandlerWritesToStdout() {
    const netproxy::LogLevel originalLevel = netproxy::Log::minLevel();
    netproxy::Log::setMinLevel(netproxy::LogLevel::Debug);
    netproxy::Log::resetHandler();

    const std::string output = captureStdout([] { netproxy::Log::info("hello ", 42); });

    test::checkTrue(contains(output, "[INFO]"), "默认处理器输出包含级别");
    test::checkTrue(contains(output, "hello 42"), "默认处理器输出包含消息");
    test::checkTrue(contains(output, "\n"), "默认处理器以换行结束");

    netproxy::Log::setMinLevel(originalLevel);
}

void customHandlerReceivesLevelAndMessage() {
    netproxy::LogLevel receivedLevel = netproxy::LogLevel::Debug;
    std::string receivedMessage;
    int callCount = 0;

    netproxy::Log::setHandler([&](netproxy::LogLevel level, const std::string& message) {
        receivedLevel = level;
        receivedMessage = message;
        ++callCount;
    });

    netproxy::Log::warning("custom ", 7);

    test::checkEquals(callCount, 1, "自定义处理器被调用一次");
    test::checkTrue(receivedLevel == netproxy::LogLevel::Warning, "自定义处理器收到 Warning 级别");
    test::checkEquals(receivedMessage, std::string("custom 7"), "自定义处理器收到完整消息");

    netproxy::Log::resetHandler();
}

void customHandlerReplacesStdout() {
    const netproxy::LogLevel originalLevel = netproxy::Log::minLevel();
    netproxy::Log::setMinLevel(netproxy::LogLevel::Debug);
    netproxy::Log::setHandler([](netproxy::LogLevel, const std::string&) {});

    const std::string output = captureStdout([] { netproxy::Log::error("hidden"); });

    test::checkTrue(output.empty(), "设置自定义处理器后不再写标准输出");

    netproxy::Log::resetHandler();
    netproxy::Log::setMinLevel(originalLevel);
}

void minLevelFiltersMessages() {
    const netproxy::LogLevel originalLevel = netproxy::Log::minLevel();
    int callCount = 0;

    netproxy::Log::setHandler([&](netproxy::LogLevel, const std::string&) { ++callCount; });
    netproxy::Log::setMinLevel(netproxy::LogLevel::Warning);

    netproxy::Log::debug("debug");
    netproxy::Log::info("info");
    netproxy::Log::warning("warning");
    netproxy::Log::error("error");

    test::checkEquals(callCount, 2, "只输出 Warning 及以上级别");
    test::checkFalse(netproxy::Log::isEnabled(netproxy::LogLevel::Info), "Info 低于最小级别时被禁用");

    netproxy::Log::setMinLevel(originalLevel);
    netproxy::Log::resetHandler();
}

} // namespace

void runLogTests() {
    NETPROXY_CASE(defaultHandlerWritesToStdout);
    NETPROXY_CASE(customHandlerReceivesLevelAndMessage);
    NETPROXY_CASE(customHandlerReplacesStdout);
    NETPROXY_CASE(minLevelFiltersMessages);
}
