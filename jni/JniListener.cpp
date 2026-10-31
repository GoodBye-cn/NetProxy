#include "JniListener.h"

#include <stdexcept>
#include <utility>

#include "JniContract.h"
#include "JniEnv.h"

namespace {

constexpr char INVALID_ARGUMENT[] = "listener is null";
constexpr char CREATE_FAILED[] = "NewGlobalRef failed";
constexpr char CLASS_FAILED[] = "GetObjectClass failed";
constexpr char METHOD_MISSING[] = "Listener is missing onPeer/onStreamEvent/onData";

} // namespace

namespace netproxy::jni {

JniListener::JniListener(JNIEnv* env, jobject listener) {
    if (env == nullptr || listener == nullptr) {
        throw std::invalid_argument(INVALID_ARGUMENT);
    }

    ref_ = env->NewGlobalRef(listener);
    if (ref_ == nullptr) {
        throw std::runtime_error(CREATE_FAILED);
    }

    jclass clazz = env->GetObjectClass(ref_);
    if (clazz == nullptr) {
        env->ExceptionClear();
        env->DeleteGlobalRef(ref_);
        ref_ = nullptr;
        throw std::runtime_error(CLASS_FAILED);
    }

    auto resolve = [env, clazz](const char* name, const char* signature) {
        jmethodID id = env->GetMethodID(clazz, name, signature);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        return id;
    };

    onPeer_ = resolve(ON_PEER_NAME, ON_PEER_SIGNATURE);
    onStreamEvent_ = resolve(ON_STREAM_EVENT_NAME, ON_STREAM_EVENT_SIGNATURE);
    onData_ = resolve(ON_DATA_NAME, ON_DATA_SIGNATURE);

    env->DeleteLocalRef(clazz);

    if (onPeer_ == nullptr || onStreamEvent_ == nullptr || onData_ == nullptr) {
        env->DeleteGlobalRef(ref_);
        ref_ = nullptr;
        throw std::runtime_error(METHOD_MISSING);
    }
}

JniListener::~JniListener() {
    if (ref_ == nullptr) {
        return;
    }

    ScopedEnv env;
    if (env) {
        env.get()->DeleteGlobalRef(ref_);
    }
    ref_ = nullptr;
}

void JniListener::onPeer(const PeerInfo& peer) {
    ScopedEnv env;
    if (!env) {
        return;
    }

    JNIEnv* jni = env.get();
    jstring address = newString(jni, peer.address);
    jni->CallVoidMethod(ref_, onPeer_, static_cast<jint>(peer.nodeId), address, static_cast<jint>(peer.port));
    if (address != nullptr) {
        jni->DeleteLocalRef(address);
    }
    clearPendingException(jni, "onPeer");
}

void JniListener::onStreamEvent(std::uint32_t streamId, std::uint32_t peerNodeId, StreamEvent event, std::uint8_t reason) {
    ScopedEnv env;
    if (!env) {
        return;
    }

    JNIEnv* jni = env.get();
    jni->CallVoidMethod(ref_, onStreamEvent_, static_cast<jint>(streamId), static_cast<jint>(peerNodeId),
                        static_cast<jint>(static_cast<int>(event)), static_cast<jint>(reason));
    clearPendingException(jni, "onStreamEvent");
}

void JniListener::onData(std::uint32_t streamId, std::uint32_t peerNodeId, const char* data, std::size_t size) {
    ScopedEnv env;
    if (!env) {
        return;
    }

    JNIEnv* jni = env.get();
    const auto length = static_cast<jsize>(size);
    jbyteArray payload = jni->NewByteArray(length);
    if (payload == nullptr) {
        clearPendingException(jni, "onData: NewByteArray");
        return;
    }

    if (data != nullptr && length > 0) {
        jni->SetByteArrayRegion(payload, 0, length, reinterpret_cast<const jbyte*>(data));
    }

    jni->CallVoidMethod(ref_, onData_, static_cast<jint>(streamId), static_cast<jint>(peerNodeId), payload);
    jni->DeleteLocalRef(payload);
    clearPendingException(jni, "onData");
}

} // namespace netproxy::jni
