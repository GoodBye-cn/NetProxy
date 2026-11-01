add_executable(netproxy_demo
    Main.cpp
)

find_package(Threads REQUIRED)

target_link_libraries(netproxy_demo
    PRIVATE
        NetProxy::netproxy
        Threads::Threads
)

netproxySetWarnings(netproxy_demo)

# Visual Studio 多配置生成器默认输出到 bin/Debug、bin/Release，统一到 bin。
set_target_properties(netproxy_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/bin
)
