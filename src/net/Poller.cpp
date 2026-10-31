#include "Poller.h"

#include "Log.h"

#include <chrono>
#include <cstring>

#ifndef _WIN32
#include <cerrno>
#endif

namespace netproxy::net {

short Poller::toNativeEvents(std::uint32_t events) {
    short native = 0;
    if ((events & Read) != 0) {
        native = static_cast<short>(native | POLLIN);
    }
    if ((events & Write) != 0) {
        native = static_cast<short>(native | POLLOUT);
    }
    return native;
}

std::uint32_t Poller::toEvents(short nativeEvents) {
    std::uint32_t events = 0;
    if ((nativeEvents & (POLLIN | POLLPRI)) != 0) {
        events |= Read;
    }
    if ((nativeEvents & POLLOUT) != 0) {
        events |= Write;
    }
    if ((nativeEvents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        events |= Error;
    }
    return events;
}

std::ptrdiff_t Poller::indexOf(SocketHandle handle) const {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (entries_[index].fd == handle) {
            return static_cast<std::ptrdiff_t>(index);
        }
    }
    return -1;
}

Poller::Poller() {
    wakeupReady_ = wakeup_.open();
    if (!wakeupReady_) {
        Log::error("Poller 初始化失败: 无法创建唤醒通道");
    }
}

Poller::~Poller() {
    wakeup_.close();
}

bool Poller::add(SocketHandle handle, std::uint32_t events) {
    if (handle == INVALID_SOCKET_HANDLE) {
        Log::warning("Poller 添加失败: 句柄无效");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (indexOf(handle) >= 0) {
            Log::warning("Poller 添加失败: 句柄已存在");
            return false;
        }

        PollDescriptor descriptor{};
        descriptor.fd = handle;
        descriptor.events = toNativeEvents(events);
        descriptor.revents = 0;
        entries_.push_back(descriptor);
    }

    wakeup_.notify();
    return true;
}

bool Poller::modify(SocketHandle handle, std::uint32_t events) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::ptrdiff_t index = indexOf(handle);
        if (index < 0) {
            Log::warning("Poller 修改失败: 句柄未注册");
            return false;
        }

        entries_[static_cast<std::size_t>(index)].events = toNativeEvents(events);
    }

    wakeup_.notify();
    return true;
}

bool Poller::remove(SocketHandle handle) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::ptrdiff_t index = indexOf(handle);
        if (index < 0) {
            Log::warning("Poller 移除失败: 句柄未注册");
            return false;
        }

        entries_.erase(entries_.begin() + index);
    }

    wakeup_.notify();
    return true;
}

bool Poller::contains(SocketHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return indexOf(handle) >= 0;
}

std::size_t Poller::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

bool Poller::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.empty();
}

void Poller::clear() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

    wakeup_.notify();
}

void Poller::wakeup() {
    wakeupRequested_.store(true);
    wakeup_.notify();
}

int Poller::poll(int timeoutMs, std::vector<ReadyEvent>& readyEvents) {
    readyEvents.clear();

    if (!wakeupReady_) {
        // 构造阶段已记录过失败原因，这里不再逐次输出，避免调用方循环时刷屏。
        return -1;
    }

    const auto start = std::chrono::steady_clock::now();
    int remainingMs = timeoutMs;

    while (true) {
        std::vector<PollDescriptor> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot = entries_;
        }

        PollDescriptor wakeupDescriptor{};
        wakeupDescriptor.fd = wakeup_.handle();
        wakeupDescriptor.events = POLLIN;
        wakeupDescriptor.revents = 0;
        snapshot.insert(snapshot.begin(), wakeupDescriptor);

#ifdef _WIN32
        const int result = ::WSAPoll(snapshot.data(), static_cast<ULONG>(snapshot.size()), remainingMs);
#else
        const int result = ::poll(snapshot.data(), static_cast<nfds_t>(snapshot.size()), remainingMs);
#endif

        if (result < 0) {
#ifndef _WIN32
            if (errno == EINTR) {
                return 0;
            }
            Log::warning("poll 失败: ", std::strerror(errno));
#else
            Log::warning("poll 失败: ", WSAGetLastError());
#endif
            return -1;
        }

        const bool wakeupReady = snapshot[0].revents != 0;
        if (wakeupReady) {
            wakeup_.drain();
        }

        for (std::size_t index = 1; index < snapshot.size(); ++index) {
            if (snapshot[index].revents == 0) {
                continue;
            }

            ReadyEvent ready;
            ready.handle = snapshot[index].fd;
            ready.events = toEvents(snapshot[index].revents);
            readyEvents.push_back(ready);
        }

        // 用户主动唤醒：立即返回，不等待业务事件。
        if (wakeupReady && readyEvents.empty() && wakeupRequested_.exchange(false)) {
            return 0;
        }

        // 有业务事件或超时无事件时结束。
        if (!readyEvents.empty() || result == 0) {
            return static_cast<int>(readyEvents.size());
        }

        // 只有注册表变更的通知：重新取快照并等待剩余时间。
        if (timeoutMs > 0) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            remainingMs = timeoutMs - static_cast<int>(elapsed.count());
            if (remainingMs <= 0) {
                return 0;
            }
        }
    }
}

} // namespace netproxy::net
