#include <atomic>
#include <chrono>
#include <thread>

#include "TestSupport.h"
#include "core/IoLoop.h"
#include "core/TaskLoop.h"

namespace {

using netproxy::core::IoLoop;
using netproxy::core::TaskLoop;

// 轮询等待条件成立，最多等待指定时间。
template <typename Predicate> bool waitFor(Predicate predicate, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

void ioLoopRunsPostedTasks() {
    IoLoop loop;
    test::checkTrue(loop.start(), "IoLoop: 启动");
    test::checkFalse(loop.start(), "IoLoop: 重复启动被拒绝");

    std::atomic<int> counter{0};
    for (int index = 0; index < 10; ++index) {
        loop.post([&counter] { counter.fetch_add(1); });
    }

    test::checkTrue(waitFor([&counter] { return counter.load() == 10; }, 2000), "IoLoop: 投递任务全部执行");

    loop.stop();
    test::checkFalse(loop.isRunning(), "IoLoop: 停止后状态正确");
}

void taskLoopRunsImmediateAndDelayed() {
    TaskLoop loop;
    test::checkTrue(loop.start(), "TaskLoop: 启动");
    test::checkFalse(loop.start(), "TaskLoop: 重复启动被拒绝");

    std::atomic<int> counter{0};
    const auto start = std::chrono::steady_clock::now();

    loop.post([&counter] { counter.fetch_add(1); });
    loop.postDelayed(50, [&counter, start] {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        if (elapsed.count() >= 40) {
            counter.fetch_add(10);
        }
    });

    test::checkTrue(waitFor([&counter] { return counter.load() >= 11; }, 2000), "TaskLoop: 立即与延迟任务都执行");

    loop.stop();
    test::checkFalse(loop.isRunning(), "TaskLoop: 停止后状态正确");
}

void taskLoopZeroDelayRunsImmediately() {
    TaskLoop loop;
    test::checkTrue(loop.start(), "TaskLoop 零延迟: 启动");

    std::atomic<int> counter{0};
    loop.postDelayed(0, [&counter] { counter.fetch_add(1); });

    test::checkTrue(waitFor([&counter] { return counter.load() == 1; }, 2000), "TaskLoop 零延迟: 立即执行");

    loop.stop();
}

} // namespace

void runCoreTests() {
    NETPROXY_CASE(ioLoopRunsPostedTasks);
    NETPROXY_CASE(taskLoopRunsImmediateAndDelayed);
    NETPROXY_CASE(taskLoopZeroDelayRunsImmediately);
}
