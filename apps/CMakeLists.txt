add_executable(netproxy_cli
    Main.cpp
)

find_package(Threads REQUIRED)

target_link_libraries(netproxy_cli
    PRIVATE
        NetProxy::netproxy
        Threads::Threads
)

netproxySetWarnings(netproxy_cli)
