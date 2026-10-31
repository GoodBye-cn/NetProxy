#pragma once

#include <jni.h>

namespace netproxy::jni {

// 向 com.open.netproxy.P2PServiceNative 注册全部 native 方法。
bool registerServiceNatives(JNIEnv* env, jclass clazz);

} // namespace netproxy::jni
