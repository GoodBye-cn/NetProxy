#pragma once

#include <functional>
#include <sstream>
#include <string>

namespace netproxy {

enum class LogLevel { Debug, Info, Warning, Error };

using LogHandler = std::function<void(LogLevel level, const std::string& message)>;

class Log final {
public:
    Log() = delete;

    // 设置日志处理器，用于重定向日志；传入空处理器等价于恢复默认。
    static void setHandler(LogHandler handler);

    // 恢复默认处理器：输出到标准输出。
    static void resetHandler();

    // 低于该级别的日志不会输出，默认 Info。
    static void setMinLevel(LogLevel level);

    static LogLevel minLevel();

    static bool isEnabled(LogLevel level);

    static void log(LogLevel level, const std::string& message);

    template <typename... Args> static void debug(const Args&... args) {
        log(LogLevel::Debug, buildMessage(args...));
    }

    template <typename... Args> static void info(const Args&... args) {
        log(LogLevel::Info, buildMessage(args...));
    }

    template <typename... Args> static void warning(const Args&... args) {
        log(LogLevel::Warning, buildMessage(args...));
    }

    template <typename... Args> static void error(const Args&... args) {
        log(LogLevel::Error, buildMessage(args...));
    }

private:
    template <typename... Args> static std::string buildMessage(const Args&... args) {
        std::ostringstream stream;
        (stream << ... << args);
        return stream.str();
    }
};

} // namespace netproxy
