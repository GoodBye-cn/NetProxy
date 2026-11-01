#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "TestSupport.h"
#include "Tests.h"

// 流层独立测试入口：不链接主库，只编入 netproxy_stream 的 objects。
int main(int argc, char* argv[]) {
    // 用例名称是 UTF-8 字面量（/utf-8），需要把控制台输出切到同一编码。
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    test::parseArguments(argc, argv);

    runStreamMuxTests();

    return test::finish("NetProxy.Stream");
}
