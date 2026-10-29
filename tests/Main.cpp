#include "Log.h"
#include "TestSupport.h"
#include "Tests.h"

int main(int argc, char* argv[]) {
    test::parseArguments(argc, argv);

    if (test::debugLogging()) {
        netproxy::Log::setMinLevel(netproxy::LogLevel::Debug);
    }

    runVersionTests();
    runLogTests();
    runSocketTests();
    runPollTests();
    runDispatcherTests();
    runDiscoveryTests();
    runNodeTests();
    runCoreTests();
    runP2PServiceTests();
    runPeerProtocolTests();
    runPeerConnectionTests();

    return test::finish("NetProxy");
}
