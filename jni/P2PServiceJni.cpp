#include "P2PServiceJni.h"

#include <arpa/inet.h>
#include <android/log.h>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "JniContract.h"
#include "JniEnv.h"
#include "JniListener.h"
#include "JniRegistry.h"
#include "Log.h"
#include "P2PService.h"

namespace netproxy::jni {
namespace {

using Entry = JniRegistry::Entry;

constexpr char ILLEGAL_STATE[] = "java/lang/IllegalStateException";
constexpr char ILLEGAL_ARGUMENT[] = "java/lang/IllegalArgumentException";
constexpr char NULL_POINTER[] = "java/lang/NullPointerException";
constexpr char LOG_TAG[] = "NetProxy";

// C++ 异常不能跨越 JNI 边界，统一转成 Java 异常。
template <typename Fn, typename Result = std::invoke_result_t<Fn>>
std::enable_if_t<!std::is_void_v<Result>, Result> jniGuard(JNIEnv* env, const char* name, Fn&& fn) {
    try {
        return fn();
    } catch (const std::exception& error) {
        throwJava(env, ILLEGAL_STATE, std::string(name) + ": " + error.what());
    } catch (...) {
        throwJava(env, ILLEGAL_STATE, std::string(name) + ": unknown native error");
    }
    return Result{};
}

template <typename Fn, typename Result = std::invoke_result_t<Fn>>
std::enable_if_t<std::is_void_v<Result>, void> jniGuard(JNIEnv* env, const char* name, Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& error) {
        throwJava(env, ILLEGAL_STATE, std::string(name) + ": " + error.what());
    } catch (...) {
        throwJava(env, ILLEGAL_STATE, std::string(name) + ": unknown native error");
    }
}

bool requireService(JNIEnv* env, jlong handle, Entry& entry) {
    entry = JniRegistry::instance().find(handle);
    if (entry.service) {
        return true;
    }
    throwJava(env, ILLEGAL_STATE, "NetProxy: invalid handle");
    return false;
}

// PeerInfo::address 是数字形式 IPv4，打包成 32 位无符号值交给 Java。
std::uint32_t ipv4Value(const std::string& address) {
    in_addr parsed{};
    if (inet_pton(AF_INET, address.c_str(), &parsed) != 1) {
        return 0;
    }
    return ntohl(parsed.s_addr);
}

int androidPriority(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return ANDROID_LOG_DEBUG;
    case LogLevel::Info:
        return ANDROID_LOG_INFO;
    case LogLevel::Warning:
        return ANDROID_LOG_WARN;
    case LogLevel::Error:
        return ANDROID_LOG_ERROR;
    }
    return ANDROID_LOG_DEFAULT;
}

jlong nativeCreate(JNIEnv* env, jclass) {
    return jniGuard(env, "nativeCreate", [] { return JniRegistry::instance().create(); });
}

void nativeDestroy(JNIEnv* env, jclass, jlong handle) {
    jniGuard(env, "nativeDestroy", [handle] { JniRegistry::instance().destroy(handle); });
}

jboolean nativeStart(JNIEnv* env, jclass, jlong handle, jint nodeId, jint dataPort, jstring bindAddress,
                     jstring multicastGroup, jint multicastPort) {
    return jniGuard(env, "nativeStart", [&]() -> jboolean {
        Entry entry;
        if (!requireService(env, handle, entry)) {
            return JNI_FALSE;
        }
        if (dataPort < 0 || dataPort > 65535 || multicastPort < 0 || multicastPort > 65535) {
            throwJava(env, ILLEGAL_ARGUMENT, "port out of range");
            return JNI_FALSE;
        }

        P2PServiceConfig config;
        config.nodeId = static_cast<std::uint32_t>(nodeId);
        config.dataPort = static_cast<std::uint16_t>(dataPort);
        if (bindAddress != nullptr) {
            config.dataBindAddress = toUtf8(env, bindAddress);
        }
        if (multicastGroup != nullptr) {
            config.multicastGroup = toUtf8(env, multicastGroup);
        }
        config.multicastPort = static_cast<std::uint16_t>(multicastPort);

        return entry.service->start(config) ? JNI_TRUE : JNI_FALSE;
    });
}

void nativeStop(JNIEnv* env, jclass, jlong handle) {
    jniGuard(env, "nativeStop", [&] {
        Entry entry;
        if (!requireService(env, handle, entry)) {
            return;
        }
        entry.service->stop();
    });
}

jboolean nativeIsRunning(JNIEnv* env, jclass, jlong handle) {
    return jniGuard(env, "nativeIsRunning", [&]() -> jboolean {
        Entry entry;
        if (!requireService(env, handle, entry)) {
            return JNI_FALSE;
        }
        return entry.service->isRunning() ? JNI_TRUE : JNI_FALSE;
    });
}

jint nativeNodeId(JNIEnv* env, jclass, jlong handle) {
    return jniGuard(env, "nativeNodeId", [&]() -> jint {
        Entry entry;
        if (!requireService(env, handle, entry)) {
            return 0;
        }
        return static_cast<jint>(entry.service->nodeId());
    });
}

jint nativeDataPort(JNIEnv* env, jclass, jlong handle) {
    return jniGuard(env, "nativeDataPort", [&]() -> jint {
        Entry entry;
        if (!requireService(env, handle, entry)) {
            return 0;
        }
        return static_cast<jint>(entry.service->dataPort());
    });
}

jlongArray nativePeers(JNIEnv* env, jclass, jlong handle) {
    return jniGuard(env, "nativePeers", [&]() -> jlongArray {
        Entry entry;
        if (!requireService(env, handle, entry)) {
            return nullptr;
        }

        const std::vector<PeerInfo> peers = entry.service->peers();
        std::vector<jlong> packed;
        packed.reserve(peers.size() * 3);
        for (const PeerInfo& peer : peers) {
            packed.push_back(static_cast<jlong>(peer.nodeId));
            packed.push_back(static_cast<jlong>(ipv4Value(peer.address)));
            packed.push_back(static_cast<jlong>(peer.port));
        }

        jlongArray result = env->NewLongArray(static_cast<jsize>(packed.size()));
        if (result == nullptr) {
            return nullptr;
        }
        if (!packed.empty()) {
            env->SetLongArrayRegion(result, 0, static_cast<jsize>(packed.size()), packed.data());
        }
        return result;
    });
}

jlongArray nativeStreams(JNIEnv* env, jclass, jlong handle) {
    return jniGuard(env, "nativeStreams", [&]() -> jlongArray {
        Entry entry;
        if (!requireService(env, handle, entry)) {
            return nullptr;
        }

        const std::vector<StreamInfo> streams = entry.service->streams();
        std::vector<jlong> packed;
        packed.reserve(streams.size() * 3);
        for (const StreamInfo& stream : streams) {
            packed.push_back(static_cast<jlong>(stream.streamId));
            packed.push_back(static_cast<jlong>(stream.peerNodeId));
            packed.push_back(stream.open ? 1 : 0);
        }

        jlongArray result = env->NewLongArray(static_cast<jsize>(packed.size()));
        if (result == nullptr) {
            return nullptr;
        }
        if (!packed.empty()) {
            env->SetLongArrayRegion(result, 0, static_cast<jsize>(packed.size()), packed.data());
        }
        return result;
    });
}

jboolean nativeConnect(JNIEnv* env, jclass, jlong handle, jint peerNodeId) {
    return jniGuard(env, "nativeConnect", [&]() -> jboolean {
        Entry entry;
        if (!requireService(env, handle, entry)) {
            return JNI_FALSE;
        }
        return entry.service->connect(static_cast<std::uint32_t>(peerNodeId)) ? JNI_TRUE : JNI_FALSE;
    });
}

jboolean nativeIsConnected(JNIEnv* env, jclass, jlong handle, jint peerNodeId) {
    return jniGuard(env, "nativeIsConnected", [&]() -> jboolean {
        Entry entry;
        if (!requireService(env, handle, entry)) {
            return JNI_FALSE;
        }
        return entry.service->isConnected(static_cast<std::uint32_t>(peerNodeId)) ? JNI_TRUE : JNI_FALSE;
    });
}

jint nativeOpenStream(JNIEnv* env, jclass, jlong handle, jint peerNodeId) {
    return jniGuard(env, "nativeOpenStream", [&]() -> jint {
        Entry entry;
        if (!requireService(env, handle, entry)) {
            return 0;
        }
        return static_cast<jint>(entry.service->openStream(static_cast<std::uint32_t>(peerNodeId)));
    });
}

jboolean nativeCloseStream(JNIEnv* env, jclass, jlong handle, jint streamId) {
    return jniGuard(env, "nativeCloseStream", [&]() -> jboolean {
        Entry entry;
        if (!requireService(env, handle, entry)) {
            return JNI_FALSE;
        }
        return entry.service->closeStream(static_cast<std::uint32_t>(streamId)) ? JNI_TRUE : JNI_FALSE;
    });
}

jboolean nativeSendData(JNIEnv* env, jclass, jlong handle, jint streamId, jbyteArray data) {
    return jniGuard(env, "nativeSendData", [&]() -> jboolean {
        Entry entry;
        if (!requireService(env, handle, entry)) {
            return JNI_FALSE;
        }
        if (data == nullptr) {
            throwJava(env, NULL_POINTER, "data is null");
            return JNI_FALSE;
        }

        const jsize length = env->GetArrayLength(data);
        if (length <= 0) {
            return JNI_FALSE;
        }

        jbyte* bytes = env->GetByteArrayElements(data, nullptr);
        if (bytes == nullptr) {
            return JNI_FALSE;
        }
        const bool sent = entry.service->sendData(static_cast<std::uint32_t>(streamId), bytes,
                                                  static_cast<std::size_t>(length));
        env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
        return sent ? JNI_TRUE : JNI_FALSE;
    });
}

jboolean nativeIsStreamOpen(JNIEnv* env, jclass, jlong handle, jint streamId) {
    return jniGuard(env, "nativeIsStreamOpen", [&]() -> jboolean {
        Entry entry;
        if (!requireService(env, handle, entry)) {
            return JNI_FALSE;
        }
        return entry.service->isStreamOpen(static_cast<std::uint32_t>(streamId)) ? JNI_TRUE : JNI_FALSE;
    });
}

void nativeSetListener(JNIEnv* env, jclass, jlong handle, jobject listener) {
    jniGuard(env, "nativeSetListener", [&] {
        Entry entry;
        if (!requireService(env, handle, entry)) {
            return;
        }

        if (listener == nullptr) {
            entry.service->setPeerHandler(nullptr);
            entry.service->setStreamHandler(nullptr);
            entry.service->setDataHandler(nullptr);
            JniRegistry::instance().setListener(handle, nullptr);
            return;
        }

        std::shared_ptr<JniListener> bridge;
        try {
            bridge = std::make_shared<JniListener>(env, listener);
        } catch (const std::exception& error) {
            throwJava(env, ILLEGAL_ARGUMENT, error.what());
            return;
        }

        entry.service->setPeerHandler([bridge](const PeerInfo& peer) { bridge->onPeer(peer); });
        entry.service->setStreamHandler(
            [bridge](std::uint32_t streamId, std::uint32_t peerNodeId, StreamEvent event, std::uint8_t reason) {
                bridge->onStreamEvent(streamId, peerNodeId, event, reason);
            });
        entry.service->setDataHandler(
            [bridge](std::uint32_t streamId, std::uint32_t peerNodeId, const char* data, std::size_t size) {
                bridge->onData(streamId, peerNodeId, data, size);
            });

        JniRegistry::instance().setListener(handle, std::move(bridge));
    });
}

void nativeSetLogcatEnabled(JNIEnv* env, jclass, jlong handle, jboolean enabled) {
    jniGuard(env, "nativeSetLogcatEnabled", [&] {
        Entry entry;
        if (!requireService(env, handle, entry)) {
            return;
        }

        if (enabled == JNI_TRUE) {
            Log::setHandler([](LogLevel level, const std::string& message) {
                __android_log_write(androidPriority(level), LOG_TAG, message.c_str());
            });
        } else {
            Log::resetHandler();
        }
    });
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

const JNINativeMethod NATIVE_METHODS[] = {
    {const_cast<char*>("nativeCreate"), const_cast<char*>("()J"), reinterpret_cast<void*>(&nativeCreate)},
    {const_cast<char*>("nativeDestroy"), const_cast<char*>("(J)V"), reinterpret_cast<void*>(&nativeDestroy)},
    {const_cast<char*>("nativeStart"), const_cast<char*>("(JIILjava/lang/String;Ljava/lang/String;I)Z"),
     reinterpret_cast<void*>(&nativeStart)},
    {const_cast<char*>("nativeStop"), const_cast<char*>("(J)V"), reinterpret_cast<void*>(&nativeStop)},
    {const_cast<char*>("nativeIsRunning"), const_cast<char*>("(J)Z"), reinterpret_cast<void*>(&nativeIsRunning)},
    {const_cast<char*>("nativeNodeId"), const_cast<char*>("(J)I"), reinterpret_cast<void*>(&nativeNodeId)},
    {const_cast<char*>("nativeDataPort"), const_cast<char*>("(J)I"), reinterpret_cast<void*>(&nativeDataPort)},
    {const_cast<char*>("nativePeers"), const_cast<char*>("(J)[J"), reinterpret_cast<void*>(&nativePeers)},
    {const_cast<char*>("nativeStreams"), const_cast<char*>("(J)[J"), reinterpret_cast<void*>(&nativeStreams)},
    {const_cast<char*>("nativeConnect"), const_cast<char*>("(JI)Z"), reinterpret_cast<void*>(&nativeConnect)},
    {const_cast<char*>("nativeIsConnected"), const_cast<char*>("(JI)Z"), reinterpret_cast<void*>(&nativeIsConnected)},
    {const_cast<char*>("nativeOpenStream"), const_cast<char*>("(JI)I"), reinterpret_cast<void*>(&nativeOpenStream)},
    {const_cast<char*>("nativeCloseStream"), const_cast<char*>("(JI)Z"), reinterpret_cast<void*>(&nativeCloseStream)},
    {const_cast<char*>("nativeSendData"), const_cast<char*>("(JI[B)Z"), reinterpret_cast<void*>(&nativeSendData)},
    {const_cast<char*>("nativeIsStreamOpen"), const_cast<char*>("(JI)Z"),
     reinterpret_cast<void*>(&nativeIsStreamOpen)},
    {const_cast<char*>("nativeSetListener"), const_cast<char*>(SET_LISTENER_SIGNATURE),
     reinterpret_cast<void*>(&nativeSetListener)},
    {const_cast<char*>("nativeSetLogcatEnabled"), const_cast<char*>("(JZ)V"),
     reinterpret_cast<void*>(&nativeSetLogcatEnabled)},
};

#pragma GCC diagnostic pop

} // namespace

bool registerServiceNatives(JNIEnv* env, jclass clazz) {
    if (env == nullptr || clazz == nullptr) {
        return false;
    }
    const jint count = static_cast<jint>(sizeof(NATIVE_METHODS) / sizeof(NATIVE_METHODS[0]));
    return env->RegisterNatives(clazz, NATIVE_METHODS, count) == JNI_OK;
}

} // namespace netproxy::jni
