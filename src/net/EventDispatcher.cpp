#include "EventDispatcher.h"

#include "Log.h"

#include <utility>
#include <vector>

namespace netproxy::net {

bool EventDispatcher::add(SocketHandle handle, std::uint32_t events, Handler handler) {
    if (handle == INVALID_SOCKET_HANDLE) {
        Log::warning("dispatcher add failed: invalid handle");
        return false;
    }

    if (!handler) {
        Log::warning("dispatcher add failed: empty callback");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (handlers_.find(handle) != handlers_.end()) {
            Log::warning("dispatcher add failed: handle already registered");
            return false;
        }
    }

    if (!poller_.add(handle, events)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    handlers_[handle] = std::move(handler);
    return true;
}

bool EventDispatcher::modify(SocketHandle handle, std::uint32_t events) {
    return poller_.modify(handle, events);
}

bool EventDispatcher::remove(SocketHandle handle) {
    const bool removed = poller_.remove(handle);

    std::lock_guard<std::mutex> lock(mutex_);
    handlers_.erase(handle);
    return removed;
}

bool EventDispatcher::contains(SocketHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handlers_.find(handle) != handlers_.end();
}

std::size_t EventDispatcher::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handlers_.size();
}

void EventDispatcher::clear() {
    poller_.clear();

    std::lock_guard<std::mutex> lock(mutex_);
    handlers_.clear();
}

void EventDispatcher::wakeup() {
    poller_.wakeup();
}

int EventDispatcher::dispatch(int timeoutMs) {
    std::vector<Poller::ReadyEvent> events;
    const int ready = poller_.poll(timeoutMs, events);
    if (ready <= 0) {
        return ready;
    }

    int dispatched = 0;
    for (const Poller::ReadyEvent& event : events) {
        Handler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto entry = handlers_.find(event.handle);
            if (entry == handlers_.end()) {
                continue;
            }
            handler = entry->second;
        }

        handler(event.handle, event.events);
        ++dispatched;
    }

    return dispatched;
}

} // namespace netproxy::net
