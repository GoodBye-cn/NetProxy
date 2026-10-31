#pragma once

#include <jni.h>

#include <cstddef>
#include <cstdint>

#include "P2PService.h"

namespace netproxy::jni {

// 持有 Java Listener 的全局引用并缓存方法 ID，可在业务线程分发回调。
// 构造失败会抛 std::exception（由调用方转成 Java 异常）。
class JniListener final {
public:
    JniListener(JNIEnv* env, jobject listener);
    ~JniListener();

    JniListener(const JniListener&) = delete;
    JniListener& operator=(const JniListener&) = delete;

    void onPeer(const PeerInfo& peer);
    void onStreamEvent(std::uint32_t streamId, std::uint32_t peerNodeId, StreamEvent event, std::uint8_t reason);
    void onData(std::uint32_t streamId, std::uint32_t peerNodeId, const char* data, std::size_t size);

private:
    jobject ref_ = nullptr;
    jmethodID onPeer_ = nullptr;
    jmethodID onStreamEvent_ = nullptr;
    jmethodID onData_ = nullptr;
};

} // namespace netproxy::jni
