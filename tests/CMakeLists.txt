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
    transport/connection/PeerConnectionTest.cpp
    transport/connection/PeerProtocolTest.cpp
    net/SocketTest.cpp
)

find_package(Threads REQUIRED)

target_link_libraries(netproxy_tests PRIVATE NetProxy::netproxy Threads::Threads)

# net 模块不对外暴露，测试直接引用其内部头文件
target_include_directories(netproxy_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/protocol
        ${PROJECT_SOURCE_DIR}/src/net
        ${PROJECT_SOURCE_DIR}/src/transport/connection
        ${PROJECT_SOURCE_DIR}/src/transport/stream
)

netproxySetWarnings(netproxy_tests)

add_test(NAME NetProxy.UnitTests COMMAND netproxy_tests)

# 流层独立测试：把 OBJECT 库的 objects 直接编入，不新增库产物，
# 也不链接主库——反向依赖会在链接期暴露。
add_executable(netproxy_stream_tests
    StreamMain.cpp
    transport/stream/StreamMuxTest.cpp
    $<TARGET_OBJECTS:netproxy_stream>
)

target_link_libraries(netproxy_stream_tests PRIVATE Threads::Threads)

target_include_directories(netproxy_stream_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/src/protocol
        ${PROJECT_SOURCE_DIR}/src/transport/stream
)

netproxySetWarnings(netproxy_stream_tests)

add_test(NAME NetProxy.StreamTests COMMAND netproxy_stream_tests)
