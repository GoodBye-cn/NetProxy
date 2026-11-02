# ---------------------------------------------------------------- JNI 桥接层
# 方案 B：JNI 源码直接并入 netproxy 目标，Android 出库只有一个 .so。
# 约束：仅 Android 交叉编译 + BUILD_SHARED_LIBS=ON。
if(NOT ANDROID)
    message(FATAL_ERROR "the jni directory is only for Android cross-compilation")
endif()

if(NOT BUILD_SHARED_LIBS)
    message(FATAL_ERROR "NETPROXY_BUILD_JNI requires BUILD_SHARED_LIBS=ON to produce a single .so")
endif()

set(NETPROXY_JNI_SOURCES
    JniEnv.cpp
    JniListener.cpp
    JniRegistry.cpp
    P2PServiceJni.cpp
    JniOnLoad.cpp
)

# OBJECT 库承载 JNI 源码：只对本层隐藏符号，核心库的 C++ 导出不受影响，
# JNIEXPORT 标记的 JNI_OnLoad / JNI_OnUnload 仍然可见。
add_library(netproxy_jni_objects OBJECT ${NETPROXY_JNI_SOURCES})
set_target_properties(netproxy_jni_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(netproxy_jni_objects PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(netproxy_jni_objects PRIVATE cxx_std_17)
target_compile_options(netproxy_jni_objects PRIVATE -fvisibility=hidden)
netproxySetWarnings(netproxy_jni_objects)

target_sources(netproxy PRIVATE
    JniContract.h
    JniEnv.h
    JniListener.h
    JniRegistry.h
    P2PServiceJni.h
    $<TARGET_OBJECTS:netproxy_jni_objects>
)

# 回调与 logcat 日志桥接需要 liblog。
find_library(NETPROXY_ANDROID_LOG_LIB log)
if(NOT NETPROXY_ANDROID_LOG_LIB)
    message(FATAL_ERROR "liblog not found in the NDK sysroot")
endif()

target_link_libraries(netproxy PRIVATE ${NETPROXY_ANDROID_LOG_LIB})
