#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "Task.h"
#include "net/EventDispatcher.h"

namespace netproxy::core {

/**
 * I/O 线程的主循环。
 *
 * 内部持有一个 EventDispatcher，所有 socket 的读写在 dispatch() 触发的回调中执行。
 * 其它线程通过 post() 投递任务，投递会唤醒阻塞中的 poll。
 */
class IoLoop final {
public:
    // 构造循环，不启动线程。
    IoLoop();

    // 析构时停止线程。
    ~IoLoop();

    IoLoop(const IoLoop&) = delete;
    IoLoop& operator=(const IoLoop&) = delete;

    /**
     * 启动 I/O 线程。
     *
     * @return 启动成功返回 true；已在运行返回 false。
     */
    bool start();

    /**
     * 停止 I/O 线程并 join。
     *
     * @note 不能在 I/O 线程自身的回调中调用，否则会等待自己。
     */
    void stop();

    /**
     * 投递任务到 I/O 线程，可在任意线程调用。
     *
     * @param task 待执行任务，会在下一次循环迭代中执行。
     */
    void post(Task task);

    /**
     * 获取事件分发器。
     *
     * @return 内部 EventDispatcher 引用；只能由 I/O 线程或启动前使用。
     */
    net::EventDispatcher& dispatcher();

    /**
     * 判断线程是否在运行。
     *
     * @return 已启动且未停止返回 true。
     */
    bool isRunning() const;

private:
    void run();
    void runPendingTasks();

    net::EventDispatcher dispatcher_; // 事件分发器，只在 I/O 线程使用
    std::mutex mutex_;                // 保护 tasks_
    std::deque<Task> tasks_;          // 待执行任务队列
    std::atomic<bool> running_{false};
    std::thread thread_; // I/O 线程
};

} // namespace netproxy::core
