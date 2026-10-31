package com.open.netproxy;

/**
 * 流快照，不可变。
 *
 * <p>通过 {@link P2PService#streams()} 获取，用于观察当前有哪些流处于已打开状态。
 */
public final class StreamInfo {

    private final int streamId;
    private final int peerNodeId;
    private final boolean open;

    /**
     * @param streamId   流标识（无符号 32 位）
     * @param peerNodeId 对端节点标识（无符号 32 位）
     * @param open       是否已打开
     */
    public StreamInfo(int streamId, int peerNodeId, boolean open) {
        this.streamId = streamId;
        this.peerNodeId = peerNodeId;
        this.open = open;
    }

    static StreamInfo fromPacked(int streamId, int peerNodeId, boolean open) {
        return new StreamInfo(streamId, peerNodeId, open);
    }

    /** @return 流标识（无符号 32 位） */
    public int getStreamId() {
        return streamId;
    }

    /** @return 对端节点标识（无符号 32 位） */
    public int getPeerNodeId() {
        return peerNodeId;
    }

    /** @return 流是否已打开 */
    public boolean isOpen() {
        return open;
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) {
            return true;
        }
        if (!(other instanceof StreamInfo)) {
            return false;
        }
        StreamInfo that = (StreamInfo) other;
        return streamId == that.streamId && peerNodeId == that.peerNodeId && open == that.open;
    }

    @Override
    public int hashCode() {
        int result = streamId;
        result = 31 * result + peerNodeId;
        result = 31 * result + (open ? 1 : 0);
        return result;
    }

    @Override
    public String toString() {
        return "StreamInfo{streamId=" + (streamId & 0xFFFFFFFFL)
                + ", peerNodeId=" + (peerNodeId & 0xFFFFFFFFL)
                + ", open=" + open + '}';
    }
}
