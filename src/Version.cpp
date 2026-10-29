#include "Version.h"

namespace netproxy {

namespace {

constexpr int MAJOR = NETPROXY_VERSION_MAJOR;
constexpr int MINOR = NETPROXY_VERSION_MINOR;
constexpr int PATCH = NETPROXY_VERSION_PATCH;

} // namespace

std::string Version::toString() {
    return NETPROXY_VERSION_STRING;
}

int Version::major() {
    return MAJOR;
}

int Version::minor() {
    return MINOR;
}

int Version::patch() {
    return PATCH;
}

} // namespace netproxy
