#pragma once

#include <cstring>
#include <iostream>
#include <string>

namespace test {

inline int& failureCount() {
    static int count = 0;
    return count;
}

inline void reportFailure(const char* expression, const std::string& expected, const std::string& actual) {
    std::cerr << "FAILED: " << expression << "\n"
              << "  expected: " << expected << "\n"
              << "  actual:   " << actual << "\n";
    ++failureCount();
}

inline void checkEquals(const std::string& actual, const std::string& expected, const char* expression) {
    if (actual != expected) {
        reportFailure(expression, expected, actual);
    }
}

inline void checkEquals(int actual, int expected, const char* expression) {
    if (actual != expected) {
        reportFailure(expression, std::to_string(expected), std::to_string(actual));
    }
}

inline void checkTrue(bool condition, const char* expression) {
    if (!condition) {
        reportFailure(expression, "true", "false");
    }
}

inline void checkFalse(bool condition, const char* expression) {
    if (condition) {
        reportFailure(expression, "false", "true");
    }
}

// 是否输出每个用例的执行状态，由 --verbose / -v 打开。
inline bool& verbose() {
    static bool enabled = false;
    return enabled;
}

// 是否开启 Debug 级别日志，由 --log-debug 打开。
inline bool& debugLogging() {
    static bool enabled = false;
    return enabled;
}

// 解析命令行参数，支持 --verbose / -v 与 --log-debug。
inline void parseArguments(int argc, char* argv[]) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--verbose") == 0 || std::strcmp(argv[index], "-v") == 0) {
            verbose() = true;
        } else if (std::strcmp(argv[index], "--log-debug") == 0) {
            debugLogging() = true;
        }
    }
}

// 执行单个用例，--verbose 时打印开始与结果。
inline void runCase(const char* name, void (*body)()) {
    if (verbose()) {
        std::cout << "[ RUN      ] " << name << std::endl;
    }

    const int before = failureCount();
    body();
    const bool passed = failureCount() == before;

    if (verbose()) {
        std::cout << (passed ? "[       OK ] " : "[  FAILED  ] ") << name << std::endl;
    }
}

inline int finish(const char* suite) {
    if (failureCount() == 0) {
        std::cout << suite << ": all checks passed.\n";
        return 0;
    }

    std::cerr << suite << ": " << failureCount() << " check(s) failed.\n";
    return 1;
}

} // namespace test

// 在模块入口中标记一个用例，--verbose 时打印执行状态。
#define NETPROXY_CASE(name) ::test::runCase(#name, name)
