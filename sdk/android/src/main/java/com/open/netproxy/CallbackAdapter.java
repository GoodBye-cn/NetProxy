package com.open.netproxy;

/**
 * 把 native 原始回调转发给公开监听器，包私有。
 *
 * <p>职责：
 * <ul>
 *   <li>事件码到 {@link StreamEvent} 的映射；</li>
 *   <li>捕获用户回调抛出的 {@link RuntimeException}，避免中断 native 线程；</li>
 *   <li>数据为空等异常情况的兜底与日志。</li>
 * </ul>
 */
final class CallbackAdapter implements P2PServiceNative.Listener {

    private static final byte[] EMPTY = new byte[0];

    private final P2PServiceListener target;

    CallbackAdapter(P2PServiceListener target) {
        this.target = target;
    }

    @Override
    public void onPeer(int nodeId, String address, int port) {
        if (target == null) {
            return;
        }
        try {
            target.onPeer(new PeerInfo(nodeId, address, port));
        } catch (RuntimeException error) {
            error.printStackTrace();
        }
    }

    @Override
    public void onStreamEvent(int streamId, int peerNodeId, int event, int reason) {
        if (target == null) {
            return;
        }

        StreamEvent mapped = StreamEvent.fromCode(event);
        if (mapped == null) {
            System.err.println("P2PService: unknown stream event code " + event + ", fallback to REJECTED");
            mapped = StreamEvent.REJECTED;
        }

        try {
            target.onStreamEvent(streamId, peerNodeId, mapped, reason);
        } catch (RuntimeException error) {
            error.printStackTrace();
        }
    }

    @Override
    public void onData(int streamId, int peerNodeId, byte[] data) {
        if (target == null) {
            return;
        }
        try {
            target.onData(streamId, peerNodeId, data == null ? EMPTY : data);
        } catch (RuntimeException error) {
            error.printStackTrace();
        }
    }
}
