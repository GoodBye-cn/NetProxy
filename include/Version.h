#pragma once

#include <string>

namespace netproxy {

/**
 * 编译期版本号，值来自 CMake 的 PROJECT_VERSION。
 *
 * 只读：所有方法返回编译时写入的常量，不受运行期配置影响。
 */
class Version final {
public:
    Version() = delete;

    /**
     * 获取语义化版本字符串。
     *
     * @return 形如 "1.2.3" 的字符串。
     */
    static std::string toString();

    /**
     * 获取主版本号。
     *
     * @return 编译期的 PROJECT_VERSION_MAJOR。
     */
    static int major();

    /**
     * 获取次版本号。
     *
     * @return 编译期的 PROJECT_VERSION_MINOR。
     */
    static int minor();

    /**
     * 获取修订号。
     *
     * @return 编译期的 PROJECT_VERSION_PATCH。
     */
    static int patch();
};

} // namespace netproxy
