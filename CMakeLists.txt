cmake_minimum_required(VERSION 3.20)

project(NetProxy VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

# ------------------------------------------------------------------ 全局设置
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(NETPROXY_BUILD_APPS "Build NetProxy applications" ON)
option(NETPROXY_BUILD_TESTS "Build NetProxy tests" ON)

# ------------------------------------------------------------------ 工具链检查
if(WIN32 AND NOT MSVC)
    message(WARNING "NetProxy on Windows is expected to be built with MSVC.")
elseif(NOT WIN32 AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "NetProxy on Unix-like systems is expected to be built with Clang.")
endif()

# ------------------------------------------------------------------ 公共函数
function(netproxySetWarnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# ------------------------------------------------------------------ 子项目
add_subdirectory(src)

if(NETPROXY_BUILD_APPS)
    add_subdirectory(apps)
endif()

if(NETPROXY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ------------------------------------------------------------------ 安装
install(TARGETS netproxy
    EXPORT NetProxyTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT NetProxyTargets
    FILE NetProxyTargets.cmake
    NAMESPACE NetProxy::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NetProxy
)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/NetProxyConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/NetProxyConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NetProxy
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/NetProxyConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/NetProxyConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/NetProxyConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/NetProxy
)
