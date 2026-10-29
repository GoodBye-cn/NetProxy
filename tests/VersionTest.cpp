#include <string>

#include "TestSupport.h"
#include "Version.h"

namespace {

void versionInfo() {
    const std::string expected = std::to_string(netproxy::Version::major()) + "." +
                                 std::to_string(netproxy::Version::minor()) + "." +
                                 std::to_string(netproxy::Version::patch());

    test::checkEquals(netproxy::Version::toString(), expected, "Version::toString()");
    test::checkEquals(netproxy::Version::major(), 0, "Version::major()");
}

} // namespace

void runVersionTests() {
    NETPROXY_CASE(versionInfo);
}
