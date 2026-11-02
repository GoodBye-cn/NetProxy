#pragma once

#include <type_traits>
#include <utility>

namespace netproxy::core {

/**
 * 把成员函数绑定成回调，参数由调用方在调用时提供。
 *
 * 用于替代「lambda 体只有一次成员调用」的写法：
 * `[this](const protocol::PeerFrame& frame) { streams_.onFrame(frame); }`
 * 可写成 `core::bindFront<&StreamMux::onFrame>(&streams_)`。
 *
 * @tparam Method 成员函数指针，需在调用点可见；私有成员在同类内绑定是合法的。
 * @tparam Owner 目标对象类型，由实参推导，含 const 限定。
 * @param owner 目标对象指针，存活期必须覆盖回调的使用期。
 * @return 捕获 owner 的泛型 lambda，可隐式转换为签名一致的 `std::function`。
 * @note 参数个数与类型必须与目标回调完全一致，不一致时在转换为 `std::function` 处报错。
 * @note 只保存裸指针，不延长对象寿命；C++20 起等价于 `std::bind_front`。
 */
template <auto Method, typename Owner> auto bindFront(Owner* owner) {
    static_assert(std::is_member_function_pointer_v<decltype(Method)>,
                  "bindFront requires a pointer to member function");

    // 闭包只捕获对象指针，调用时把实参原样转发给目标成员函数。
    auto bound = [owner](auto&&... args) -> decltype(auto) {
        return (owner->*Method)(std::forward<decltype(args)>(args)...);
    };
    return bound;
}

} // namespace netproxy::core
