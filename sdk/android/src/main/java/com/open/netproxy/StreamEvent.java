package com.open.netproxy;

/**
 * 流事件类型。
 *
 * <p>枚举顺序必须与 C++ 侧 {@code netproxy::StreamEvent} 保持一致：
 * {@code Opened = 0}、{@code Rejected = 1}、{@code Closed = 2}。
 * 调整顺序会破坏 JNI 回调的数值映射。
 */
public enum StreamEvent {

    /** 流已打开，可以发送数据。 */
    OPENED,

    /** 建连失败、达到流上限或对端拒绝。 */
    REJECTED,

    /** 流已关闭。 */
    CLOSED;

    private static final StreamEvent[] VALUES = values();

    /**
     * 把 native 事件码转换为枚举。
     *
     * @param code native 侧事件码
     * @return 对应枚举，未知码返回 {@code null}
     */
    static StreamEvent fromCode(int code) {
        if (code < 0 || code >= VALUES.length) {
            return null;
        }
        return VALUES[code];
    }
}
