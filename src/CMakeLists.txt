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
    session/DataChannel.cpp
    session/SessionManager.cpp
    session/SessionProtocol.cpp
    session/SessionTable.cpp
)

find_package(Threads REQUIRED)

add_library(NetProxy::netproxy ALIAS netproxy)

target_include_directories(netproxy
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_include_directories(netproxy
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
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

netproxySetWarnings(netproxy)
