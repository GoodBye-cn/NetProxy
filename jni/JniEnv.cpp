#include "JniEnv.h"

#include <android/log.h>

namespace {

JavaVM* gVm = nullptr;
constexpr char LOG_TAG[] = "NetProxyJni";

} // namespace

namespace netproxy::jni {

void setJavaVm(JavaVM* vm) {
    gVm = vm;
}

JavaVM* javaVm() {
    return gVm;
}

ScopedEnv::ScopedEnv() {
    if (gVm == nullptr) {
        return;
    }

    void* existing = nullptr;
    const jint state = gVm->GetEnv(&existing, JNI_VERSION_1_6);
    if (state == JNI_OK) {
        env_ = static_cast<JNIEnv*>(existing);
        return;
    }
    if (state != JNI_EDETACHED) {
        return;
    }

    JNIEnv* attached = nullptr;
    if (gVm->AttachCurrentThread(&attached, nullptr) != JNI_OK) {
        return;
    }
    env_ = attached;
    attached_ = true;
}

ScopedEnv::~ScopedEnv() {
    if (!attached_ || gVm == nullptr) {
        return;
    }
    gVm->DetachCurrentThread();
}

std::string toUtf8(JNIEnv* env, jstring value) {
    if (env == nullptr || value == nullptr) {
        return {};
    }

    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr) {
        return {};
    }

    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

jstring newString(JNIEnv* env, const std::string& value) {
    if (env == nullptr) {
        return nullptr;
    }
    return env->NewStringUTF(value.c_str());
}

void throwJava(JNIEnv* env, const char* className, const std::string& message) {
    if (env == nullptr) {
        return;
    }

    jclass clazz = env->FindClass(className);
    if (clazz == nullptr) {
        env->ExceptionClear();
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "FindClass(%s) failed", className);
        return;
    }

    env->ThrowNew(clazz, message.c_str());
    env->DeleteLocalRef(clazz);
}

void clearPendingException(JNIEnv* env, const char* where) {
    if (env == nullptr || !env->ExceptionCheck()) {
        return;
    }
    env->ExceptionClear();
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Java exception in callback: %s", where);
}

} // namespace netproxy::jni
