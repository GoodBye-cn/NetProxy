#include "Log.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>

namespace netproxy {

namespace {

std::mutex& stateMutex() {
    static std::mutex mutex;
    return mutex;
}

std::mutex& outputMutex() {
    static std::mutex mutex;
    return mutex;
}

LogHandler& customHandler() {
    static LogHandler handler;
    return handler;
}

LogLevel& minLevelStorage() {
    static LogLevel level = LogLevel::Info;
    return level;
}

const char* levelName(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "UNKNOWN";
}

std::tm localTime(std::time_t seconds) {
    std::tm result{};
#ifdef _MSC_VER
    localtime_s(&result, &seconds);
#else
    localtime_r(&seconds, &result);
#endif
    return result;
}

std::string formatTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::tm tm = localTime(seconds);

    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
           << milliseconds.count();
    return stream.str();
}

void writeToStdout(LogLevel level, const std::string& message) {
    std::cout << '[' << formatTimestamp() << "] [" << levelName(level) << "] " << message << '\n';
    std::cout.flush();
}

} // namespace

void Log::setHandler(LogHandler handler) {
    std::lock_guard<std::mutex> lock(stateMutex());
    customHandler() = std::move(handler);
}

void Log::resetHandler() {
    setHandler(LogHandler());
}

void Log::setMinLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(stateMutex());
    minLevelStorage() = level;
}

LogLevel Log::minLevel() {
    std::lock_guard<std::mutex> lock(stateMutex());
    return minLevelStorage();
}

bool Log::isEnabled(LogLevel level) {
    std::lock_guard<std::mutex> lock(stateMutex());
    return static_cast<int>(level) >= static_cast<int>(minLevelStorage());
}

void Log::log(LogLevel level, const std::string& message) {
    LogHandler handler;
    {
        std::lock_guard<std::mutex> lock(stateMutex());
        if (static_cast<int>(level) < static_cast<int>(minLevelStorage())) {
            return;
        }
        handler = customHandler();
    }

    std::lock_guard<std::mutex> lock(outputMutex());
    if (handler) {
        handler(level, message);
    } else {
        writeToStdout(level, message);
    }
}

} // namespace netproxy
