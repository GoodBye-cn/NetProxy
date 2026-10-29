add_executable(netproxy_tests
    LogTest.cpp
    Main.cpp
    P2PServiceTest.cpp
    VersionTest.cpp
    core/CoreTest.cpp
    net/EventDispatcherTest.cpp
    net/PollerTest.cpp
    node/NodeDiscoveryTest.cpp
    node/NodeManagerTest.cpp
    transport/PeerConnectionTest.cpp
    transport/PeerProtocolTest.cpp
    net/SocketTest.cpp
)

find_package(Threads REQUIRED)

target_link_libraries(netproxy_tests PRIVATE NetProxy::netproxy Threads::Threads)

# net 模块不对外暴露，测试直接引用其内部头文件
target_include_directories(netproxy_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src
)

netproxySetWarnings(netproxy_tests)

add_test(NAME NetProxy.UnitTests COMMAND netproxy_tests)
