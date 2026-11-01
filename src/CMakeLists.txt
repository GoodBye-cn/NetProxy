# ------------------------------------------------------------------ 传输层模块
# 两个模块用 OBJECT 库表达依赖边界：不产出独立库文件，objects 直接并入 netproxy。
# include 目录刻意只给本层与允许依赖的目录，反向 include 会在编译期失败。

add_library(netproxy_connection OBJECT
    transport/connection/PeerConnection.cpp
    transport/connection/PeerProtocol.cpp
)

target_include_directories(netproxy_connection
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/connection
        ${CMAKE_CURRENT_SOURCE_DIR}/protocol
        ${CMAKE_CURRENT_SOURCE_DIR}/net
)

target_compile_features(netproxy_connection PRIVATE cxx_std_17)
set_target_properties(netproxy_connection PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(netproxy_stream OBJECT
    transport/stream/Stream.cpp
    transport/stream/StreamMux.cpp
)

target_include_directories(netproxy_stream
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/stream
        ${CMAKE_CURRENT_SOURCE_DIR}/protocol
)

target_compile_features(netproxy_stream PRIVATE cxx_std_17)
set_target_properties(netproxy_stream PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ------------------------------------------------------------------ 主库
add_library(netproxy
    Log.cpp
    P2PService.cpp
    Version.cpp
    core/IoLoop.cpp
    core/TaskLoop.cpp
    net/EventDispatcher.cpp
    net/Poller.cpp
    net/Socket.cpp
    net/TCPSocket.cpp
    net/UDPSocket.cpp
    net/WakeupChannel.cpp
    node/NodeDiscovery.cpp
    node/NodeManager.cpp
    node/NodeProtocol.cpp
    node/PeerTable.cpp
    transport/DataChannel.cpp
    transport/PeerSession.cpp
    transport/PeerSessionManager.cpp
    $<TARGET_OBJECTS:netproxy_connection>
    $<TARGET_OBJECTS:netproxy_stream>
)

find_package(Threads REQUIRED)

add_library(NetProxy::netproxy ALIAS netproxy)

target_include_directories(netproxy
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# 组装层会包含两个模块的头，模块内部用的是短名 include，
# 因此这里需要把模块目录与 protocol/、net/ 一并加进来。
target_include_directories(netproxy
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/protocol
        ${CMAKE_CURRENT_SOURCE_DIR}/net
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/connection
        ${CMAKE_CURRENT_SOURCE_DIR}/transport/stream
)

target_compile_features(netproxy PUBLIC cxx_std_17)

target_compile_definitions(netproxy
    PRIVATE
        NETPROXY_VERSION_STRING="${PROJECT_VERSION}"
        NETPROXY_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        NETPROXY_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        NETPROXY_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

if(WIN32)
    target_link_libraries(netproxy PUBLIC ws2_32)
endif()

target_link_libraries(netproxy PRIVATE Threads::Threads)

set_target_properties(netproxy PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

netproxySetWarnings(netproxy_connection)
netproxySetWarnings(netproxy_stream)
netproxySetWarnings(netproxy)
