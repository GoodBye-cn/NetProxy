#pragma once

#include <functional>
#include <sstream>
#include <string>

namespace netproxy {

// 日志级别，数值只用于比较，不写入任何报文。
enum class LogLevel { Debug, Info, Warning, Error };

// 日志输出函数：level 为级别，message 为已拼接好的正文。
using LogHandler = std::function<void(LogLevel level, const std::string& message)>;

/**
 * 全局日志入口。
 *
 * 默认输出到标准输出；可通过 setHandler 重定向。所有方法线程安全。
 */
class Log final {
public:
    Log() = delete;

    /**
     * 设置日志处理器。
     *
     * @param handler 输出函数；传入空处理器等价于恢复默认（标准输出）。
     */
    static void setHandler(LogHandler handler);

    /**
     * 恢复默认处理器。
     *
     * @note 默认处理器输出到标准输出，不受编译期配置影响。
     */
    static void resetHandler();

    /**
     * 设置最小输出级别。
     *
     * @param level 低于该级别的日志将被丢弃；默认 Info。
     */
    static void setMinLevel(LogLevel level);

    /**
     * 获取最小输出级别。
     *
     * @return 当前阈值，默认 Info。
     */
    static LogLevel minLevel();

    /**
     * 判断某级别是否会被输出。
     *
     * @param level 待检查的级别。
     * @return 不低于 minLevel() 时返回 true。
     */
    static bool isEnabled(LogLevel level);

    /**
     * 按级别输出一条已拼好的日志。
     *
     * @param level 日志级别。
     * @param message 日志正文；低于 minLevel() 时直接丢弃。
     */
    static void log(LogLevel level, const std::string& message);

    /**
     * 输出 Debug 级别日志。
     *
     * @param args 按顺序拼接的日志片段，不插入分隔符。
     */
    template <typename... Args> static void debug(const Args&... args) {
        log(LogLevel::Debug, buildMessage(args...));
    }

    /**
     * 输出 Info 级别日志。
     *
     * @param args 按顺序拼接的日志片段，不插入分隔符。
     */
    template <typename... Args> static void info(const Args&... args) {
        log(LogLevel::Info, buildMessage(args...));
    }

    /**
     * 输出 Warning 级别日志。
     *
     * @param args 按顺序拼接的日志片段，不插入分隔符。
     */
    template <typename... Args> static void warning(const Args&... args) {
        log(LogLevel::Warning, buildMessage(args...));
    }

    /**
     * 输出 Error 级别日志。
     *
     * @param args 按顺序拼接的日志片段，不插入分隔符。
     */
    template <typename... Args> static void error(const Args&... args) {
        log(LogLevel::Error, buildMessage(args...));
    }

private:
    // 把变参逐段写入流，得到一条正文。
    template <typename... Args> static std::string buildMessage(const Args&... args) {
        std::ostringstream stream;
        (stream << ... << args);
        return stream.str();
    }
};

} // namespace netproxy
