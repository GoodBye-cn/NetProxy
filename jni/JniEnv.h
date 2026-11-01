#pragma once

#include <jni.h>

#include <string>

namespace netproxy::jni {

// 记录 JNI_OnLoad 拿到的 JavaVM，供任意线程回调使用。
void setJavaVm(JavaVM* vm);
JavaVM* javaVm();

// 当前线程的 JNIEnv：已附加直接取用；未附加则临时附加，析构时分离。
class ScopedEnv final {
public:
    ScopedEnv();
    ~ScopedEnv();

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

    JNIEnv* get() const {
        return env_;
    }
    explicit operator bool() const {
        return env_ != nullptr;
    }

private:
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};

// jstring 与 UTF-8 std::string 互转；null 输入返回空串。
std::string toUtf8(JNIEnv* env, jstring value);
jstring newString(JNIEnv* env, const std::string& value);

// 抛 Java 异常；FindClass 失败时清掉挂起异常。
void throwJava(JNIEnv* env, const char* className, const std::string& message);

// 清掉回调中未处理的 Java 异常并记录，避免带异常返回导致崩溃。
void clearPendingException(JNIEnv* env, const char* where);

} // namespace netproxy::jni
