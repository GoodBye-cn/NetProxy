package com.open.netproxy;

import java.util.Locale;

/**
 * 已发现的对端节点快照，不可变。
 *
 * <p>{@link #getNodeId()} 按无符号 32 位解释；{@link #getHost()} 为点分十进制 IPv4 地址。
 */
public final class PeerInfo {

    private final int nodeId;
    private final String host;
    private final int port;

    /**
     * @param nodeId 节点标识，按无符号 32 位解释
     * @param host   点分十进制 IPv4 地址，{@code null} 视为空串
     * @param port   对端数据端口
     */
    public PeerInfo(int nodeId, String host, int port) {
        this.nodeId = nodeId;
        this.host = host == null ? "" : host;
        this.port = port;
    }

    static PeerInfo fromPacked(int nodeId, int ipv4, int port) {
        return new PeerInfo(nodeId, formatIpv4(ipv4), port);
    }

    static String formatIpv4(int ipv4) {
        return String.format(Locale.US, "%d.%d.%d.%d",
                (ipv4 >>> 24) & 0xFF,
                (ipv4 >>> 16) & 0xFF,
                (ipv4 >>> 8) & 0xFF,
                ipv4 & 0xFF);
    }

    /** @return 节点标识（无符号 32 位） */
    public int getNodeId() {
        return nodeId;
    }

    /** @return 点分十进制 IPv4 地址 */
    public String getHost() {
        return host;
    }

    /** @return 对端数据端口 */
    public int getPort() {
        return port;
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) {
            return true;
        }
        if (!(other instanceof PeerInfo)) {
            return false;
        }
        PeerInfo that = (PeerInfo) other;
        return nodeId == that.nodeId && port == that.port && host.equals(that.host);
    }

    @Override
    public int hashCode() {
        int result = nodeId;
        result = 31 * result + host.hashCode();
        result = 31 * result + port;
        return result;
    }

    @Override
    public String toString() {
        return "PeerInfo{nodeId=" + (nodeId & 0xFFFFFFFFL) + ", host='" + host + "', port=" + port + '}';
    }
}
