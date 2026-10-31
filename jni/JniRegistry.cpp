#include "JniRegistry.h"

#include <utility>

namespace netproxy::jni {

JniRegistry& JniRegistry::instance() {
    static JniRegistry registry;
    return registry;
}

jlong JniRegistry::create() {
    auto service = std::make_shared<P2PService>();

    std::lock_guard<std::mutex> lock(mutex_);
    const jlong handle = nextHandle_++;
    entries_.emplace(handle, Entry{std::move(service), nullptr});
    return handle;
}

JniRegistry::Entry JniRegistry::find(jlong handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(handle);
    if (it == entries_.end()) {
        return {};
    }
    return it->second;
}

void JniRegistry::setListener(jlong handle, std::shared_ptr<JniListener> listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(handle);
    if (it == entries_.end()) {
        return;
    }
    it->second.listener = std::move(listener);
}

void JniRegistry::destroy(jlong handle) {
    Entry released;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = entries_.find(handle);
        if (it == entries_.end()) {
            return;
        }
        released = std::move(it->second);
        entries_.erase(it);
    }
    // released 在锁外析构：P2PService 停止线程，JniListener 释放全局引用。
}

void JniRegistry::clear() {
    std::unordered_map<jlong, Entry> released;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        released.swap(entries_);
    }
}

} // namespace netproxy::jni
