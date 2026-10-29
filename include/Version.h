#pragma once

#include <string>

namespace netproxy {

class Version final {
public:
    Version() = delete;

    static std::string toString();

    static int major();

    static int minor();

    static int patch();
};

} // namespace netproxy
