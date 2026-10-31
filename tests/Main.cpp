#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "Log.h"
#include "TestSupport.h"
#include "Tests.h"

int main(int argc, char* argv[]) {
    // 用例名称是 UTF-8 字面量（/utf-8），需要把控制台输出切到同一编码。
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

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
