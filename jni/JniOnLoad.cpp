#include <jni.h>

#include <android/log.h>

#include "JniContract.h"
#include "JniEnv.h"
#include "JniRegistry.h"
#include "P2PServiceJni.h"

namespace {

constexpr char LOG_TAG[] = "NetProxyJni";

} // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    netproxy::jni::setJavaVm(vm);

    if (vm == nullptr) {
        return JNI_ERR;
    }

    void* rawEnv = nullptr;
    if (vm->GetEnv(&rawEnv, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    JNIEnv* env = static_cast<JNIEnv*>(rawEnv);

    jclass clazz = env->FindClass(netproxy::jni::SERVICE_CLASS_NAME);
    if (clazz == nullptr) {
        env->ExceptionClear();
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "FindClass(%s) failed",
                            netproxy::jni::SERVICE_CLASS_NAME);
        return JNI_ERR;
    }

    const bool registered = netproxy::jni::registerServiceNatives(env, clazz);
    env->DeleteLocalRef(clazz);
    if (!registered) {
        env->ExceptionClear();
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "RegisterNatives failed");
        return JNI_ERR;
    }

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "JNI bridge loaded");
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* /*vm*/, void* /*reserved*/) {
    netproxy::jni::JniRegistry::instance().clear();
    netproxy::jni::setJavaVm(nullptr);
}
