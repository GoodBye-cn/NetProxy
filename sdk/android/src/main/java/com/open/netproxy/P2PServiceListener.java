package com.open.netproxy;

/**
 * SDK 回调接口。
 *
 * <p>线程约定：所有回调都在 native 业务线程执行，<b>不</b>切换到主线程。
 * 需要更新 UI 时请自行切线程。
 *
 * <p>限制：
 * <ul>
 *   <li>回调内禁止阻塞操作（网络、文件、锁等待）；</li>
 *   <li>回调内禁止调用 {@link P2PService#stop()} 或 {@link P2PService#close()}，会死锁；</li>
 *   <li>回调抛出的 {@link RuntimeException} 会被 SDK 捕获并打印堆栈，不会中断 native 线程。</li>
 * </ul>
 *
 * <p>每个方法都有空实现，按需覆盖即可。
 */
public interface P2PServiceListener {

    /**
     * 发现新节点。
     *
     * @param peer 节点信息快照
     */
    default void onPeer(PeerInfo peer) {
    }

    /**
     * 流状态变化。
     *
     * @param streamId   流标识
     * @param peerNodeId 对端节点标识
     * @param event      事件类型
     * @param reason     关闭或拒绝原因；{@link StreamEvent#OPENED} 时为 0
     */
    default void onStreamEvent(int streamId, int peerNodeId, StreamEvent event, int reason) {
    }

    /**
     * 收到对端数据。
     *
     * <p>{@code data} 由 native 层拷贝，回调返回后仍可安全持有；长度为 0 的数组表示空负载。
     *
     * @param streamId   流标识
     * @param peerNodeId 对端节点标识
     * @param data       数据内容
     */
    default void onData(int streamId, int peerNodeId, byte[] data) {
    }
}
