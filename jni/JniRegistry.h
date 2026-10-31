#pragma once

#include <jni.h>

#include <memory>
#include <mutex>
#include <unordered_map>

#include "JniListener.h"
#include "P2PService.h"

namespace netproxy::jni {

// Java 侧只持有 long 句柄，避免裸指针被复用或误用后崩溃；句柄单调递增，不复用。
class JniRegistry final {
public:
    struct Entry {
        std::shared_ptr<P2PService> service;
        std::shared_ptr<JniListener> listener;
    };

    JniRegistry(const JniRegistry&) = delete;
    JniRegistry& operator=(const JniRegistry&) = delete;

    static JniRegistry& instance();

    jlong create();
    Entry find(jlong handle) const;
    void setListener(jlong handle, std::shared_ptr<JniListener> listener);
    void destroy(jlong handle);
    void clear();

private:
    JniRegistry() = default;

    // 析构期间不进锁，避免 stop() 等待业务线程时持锁。
    mutable std::mutex mutex_;
    std::unordered_map<jlong, Entry> entries_;
    jlong nextHandle_ = 1;
};

} // namespace netproxy::jni
