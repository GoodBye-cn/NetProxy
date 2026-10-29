#include "TaskLoop.h"

#include <utility>

namespace netproxy::core {

std::chrono::steady_clock::time_point TaskLoop::nearestDeadline(const std::vector<DelayedTask>& delayed) {
    auto nearest = delayed.front().deadline;
    for (const DelayedTask& entry : delayed) {
        if (entry.deadline < nearest) {
            nearest = entry.deadline;
        }
    }
    return nearest;
}

TaskLoop::TaskLoop() = default;

TaskLoop::~TaskLoop() {
    stop();
}

bool TaskLoop::start() {
    if (running_.exchange(true)) {
        return false;
    }

    thread_ = std::thread([this] { run(); });
    return true;
}

void TaskLoop::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    condition_.notify_all();

    if (thread_.joinable()) {
        thread_.join();
    }
}

void TaskLoop::post(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
    }

    condition_.notify_one();
}

void TaskLoop::postDelayed(int delayMs, Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (delayMs <= 0) {
            tasks_.push_back(std::move(task));
        } else {
            delayed_.push_back(
                {std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs), std::move(task)});
        }
    }

    condition_.notify_one();
}

bool TaskLoop::isRunning() const {
    return running_.load();
}

void TaskLoop::run() {
    while (true) {
        std::deque<Task> ready;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!running_.load()) {
                break;
            }

            if (tasks_.empty()) {
                if (delayed_.empty()) {
                    condition_.wait(lock);
                } else {
                    condition_.wait_until(lock, nearestDeadline(delayed_));
                }
            }

            if (!running_.load()) {
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            for (auto entry = delayed_.begin(); entry != delayed_.end();) {
                if (entry->deadline <= now) {
                    tasks_.push_back(std::move(entry->task));
                    entry = delayed_.erase(entry);
                } else {
                    ++entry;
                }
            }

            ready.swap(tasks_);
        }

        for (Task& task : ready) {
            task();
        }
    }
}

} // namespace netproxy::core
