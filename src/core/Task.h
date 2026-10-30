#pragma once

#include <functional>

namespace netproxy::core {

/**
 * 无参任务。
 *
 * 由 `IoLoop::post` 或 `TaskLoop::post` 投递执行；
 * `TaskLoop::postDelayed` 使用同一类型，延迟时间由调用参数决定。
 */
using Task = std::function<void()>;

} // namespace netproxy::core
