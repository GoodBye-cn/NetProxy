#include "IoLoop.h"

#include <chrono>
#include <thread>
#include <utility>

namespace netproxy::core {

IoLoop::IoLoop() = default;

IoLoop::~IoLoop() {
    stop();
}

bool IoLoop::start() {
    if (running_.exchange(true)) {
        return false;
    }

    thread_ = std::thread([this] { run(); });
    return true;
}

void IoLoop::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    dispatcher_.wakeup();

    if (thread_.joinable()) {
        thread_.join();
    }
}

void IoLoop::post(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
    }

    dispatcher_.wakeup();
}

net::EventDispatcher& IoLoop::dispatcher() {
    return dispatcher_;
}

bool IoLoop::isRunning() const {
    return running_.load();
}

void IoLoop::run() {
    while (running_.load()) {
        if (dispatcher_.dispatch(100) < 0) {
            // 底层 poll 不可用（如唤醒通道创建失败）时避免忙等占满 CPU。
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        runPendingTasks();
    }
}

void IoLoop::runPendingTasks() {
    std::deque<Task> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(tasks_);
    }

    for (Task& task : pending) {
        task();
    }
}

} // namespace netproxy::core
