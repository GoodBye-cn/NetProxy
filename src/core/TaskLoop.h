#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "Task.h"

namespace netproxy::core {

/**
 * 业务线程的主循环。
 *
 * 负责任务队列与定时任务：post() 立即执行，postDelayed() 到期后执行。
 * 线程在无任务时阻塞等待，stop() 后退出。
 */
class TaskLoop final {
public:
    // 构造循环，不启动线程。
    TaskLoop();

    // 析构时停止线程。
    ~TaskLoop();

    TaskLoop(const TaskLoop&) = delete;
    TaskLoop& operator=(const TaskLoop&) = delete;

    /**
     * 启动业务线程。
     *
     * @return 启动成功返回 true；已在运行返回 false。
     */
    bool start();

    /**
     * 停止业务线程并 join。
     *
     * @note 不能在业务线程自身的任务中调用。
     */
    void stop();

    /**
     * 投递立即执行的任务。
     *
     * @param task 待执行任务。
     */
    void post(Task task);

    /**
     * 投递延迟执行的任务。
     *
     * @param delayMs 延迟毫秒数，小于等于 0 时立即执行。
     * @param task 待执行任务。
     */
    void postDelayed(int delayMs, Task task);

    /**
     * 判断线程是否在运行。
     *
     * @return 已启动且未停止返回 true。
     */
    bool isRunning() const;

private:
    struct DelayedTask {
        std::chrono::steady_clock::time_point deadline; // 计划执行时间
        Task task;                                      // 延迟任务体
    };

    // 取最近一个定时任务的到期时间，调用方需持有锁且队列非空。
    static std::chrono::steady_clock::time_point nearestDeadline(const std::vector<DelayedTask>& delayed);

    void run();

    std::mutex mutex_;                  // 保护任务队列与延迟队列
    std::condition_variable condition_; // 唤醒业务线程
    std::deque<Task> tasks_;            // 立即执行的任务队列
    std::vector<DelayedTask> delayed_;  // 按时间排序的延迟任务
    std::atomic<bool> running_{false};  // 是否已 start 且未 stop
    std::thread thread_;                // 业务线程
};

} // namespace netproxy::core
