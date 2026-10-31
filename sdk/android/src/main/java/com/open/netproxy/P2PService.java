package com.open.netproxy;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Objects;

/**
 * NetProxy SDK 的服务入口。
 *
 * <p>一个实例对应一个 native 服务，内部包含一个 I/O 线程与一个业务线程。
 * 本类线程安全，除 {@link #close()} 外的方法可从任意线程调用。
 *
 * <p>生命周期：
 *
 * <pre>{@code
 * P2PServiceConfig config = P2PServiceConfig.builder().setDataPort(0).build();
 * try (P2PService service = new P2PService(listener)) {
 *     service.start(config);
 *     for (PeerInfo peer : service.peers()) {
 *         service.connect(peer.getNodeId());
 *     }
 * }
 * }</pre>
 *
 * <p>线程与异常约定：
 * <ul>
 *   <li>回调在 native 业务线程触发，不在主线程，禁止阻塞；</li>
 *   <li>回调内禁止调用 {@link #stop()} / {@link #close()}，会死锁；</li>
 *   <li>{@link #stop()} 与 {@link #close()} 会阻塞等待 native 线程退出，勿在主线程调用；</li>
 *   <li>关闭后再调用业务方法会抛 {@link IllegalStateException}。</li>
 * </ul>
 */
public final class P2PService implements AutoCloseable {

    /** 单帧最大负载（字节），与 C++ 侧 {@code P2PServiceConfig::maxPayloadSize} 默认值一致。 */
    public static final int MAX_PAYLOAD_SIZE = 1400;

    private final P2PServiceNative nativeService;

    private volatile long handle;

    private volatile CallbackAdapter adapter;

    /**
     * 创建实例并加载 native 库。
     *
     * @param listener 回调监听器，可为 {@code null}，之后可用 {@link #setListener} 设置
     * @throws UnsatisfiedLinkError   native 库缺失或 ABI 不匹配
     * @throws IllegalStateException  native 句柄创建失败
     */
    public P2PService(P2PServiceListener listener) {
        this.nativeService = new P2PServiceNative();

        long created = nativeService.create();
        if (created == 0) {
            throw new IllegalStateException("P2PService: native handle creation failed");
        }
        this.handle = created;

        if (listener != null) {
            try {
                setListener(listener);
            } catch (RuntimeException error) {
                nativeService.destroy(created);
                this.handle = 0;
                throw error;
            }
        }
    }

    /**
     * 启动服务。
     *
     * <p>幂等：已启动时再次调用返回 {@code true}。启动前会应用当前的监听器与日志配置。
     *
     * @param config 启动配置
     * @return 成功返回 {@code true}
     * @throws NullPointerException  {@code config} 为 {@code null}
     * @throws IllegalStateException 已关闭
     */
    public boolean start(P2PServiceConfig config) {
        Objects.requireNonNull(config, "config");
        long current = requireOpen();

        CallbackAdapter currentAdapter = adapter;
        if (currentAdapter != null) {
            nativeService.setListener(current, currentAdapter);
        }
        nativeService.setLogcatEnabled(current, config.isLogcatEnabled());

        return nativeService.start(current,
                config.getNodeId(),
                config.getDataPort(),
                config.getBindAddress(),
                config.getMulticastGroup(),
                config.getMulticastPort());
    }

    /**
     * 停止服务并等待 native 线程退出，幂等。
     *
     * @throws IllegalStateException 已关闭
     */
    public void stop() {
        nativeService.stop(requireOpen());
    }

    /**
     * @return 服务是否处于运行状态；已关闭返回 {@code false}
     */
    public boolean isRunning() {
        long current = handle;
        return current != 0 && nativeService.isRunning(current);
    }

    /**
     * @return 当前节点标识（无符号 32 位），未启动返回 0
     * @throws IllegalStateException 已关闭
     */
    public int nodeId() {
        return nativeService.nodeId(requireOpen());
    }

    /**
     * @return 实际数据端口，未启动返回 0
     * @throws IllegalStateException 已关闭
     */
    public int dataPort() {
        return nativeService.dataPort(requireOpen());
    }

    /**
     * 已发现节点快照。
     *
     * <p>连接状态不在这里体现：{@code peers()} 只说明“发现了谁”，
     * 是否已建立连接请用 {@link #isConnected(int)} 查询。
     *
     * @return 不可变列表，未启动返回空列表
     * @throws IllegalStateException 已关闭
     */
    public List<PeerInfo> peers() {
        long[] packed = nativeService.peers(requireOpen());
        if (packed == null) {
            return Collections.emptyList();
        }
        if (packed.length % 3 != 0) {
            throw new IllegalStateException("P2PService: malformed peers payload");
        }

        List<PeerInfo> result = new ArrayList<>(packed.length / 3);
        for (int i = 0; i < packed.length; i += 3) {
            result.add(PeerInfo.fromPacked((int) packed[i], (int) packed[i + 1], (int) packed[i + 2]));
        }
        return Collections.unmodifiableList(result);
    }

    /**
     * 流快照，用于观察当前建立了哪些流。
     *
     * @return 不可变列表，未启动返回空列表
     * @throws IllegalStateException 已关闭
     */
    public List<StreamInfo> streams() {
        long[] packed = nativeService.streams(requireOpen());
        if (packed == null) {
            return Collections.emptyList();
        }
        if (packed.length % 3 != 0) {
            throw new IllegalStateException("P2PService: malformed streams payload");
        }

        List<StreamInfo> result = new ArrayList<>(packed.length / 3);
        for (int i = 0; i < packed.length; i += 3) {
            result.add(StreamInfo.fromPacked((int) packed[i], (int) packed[i + 1], packed[i + 2] != 0));
        }
        return Collections.unmodifiableList(result);
    }

    /**
     * 与指定节点建立连接，结果异步生效。
     *
     * @param peerNodeId 对端节点标识，必须已在节点表中
     * @return 请求已受理返回 {@code true}
     * @throws IllegalStateException 已关闭
     */
    public boolean connect(int peerNodeId) {
        return nativeService.connect(requireOpen(), peerNodeId);
    }

    /**
     * @param peerNodeId 对端节点标识
     * @return 连接是否已建立
     * @throws IllegalStateException 已关闭
     */
    public boolean isConnected(int peerNodeId) {
        return nativeService.isConnected(requireOpen(), peerNodeId);
    }

    /**
     * 在指定节点上开一条流，结果通过 {@link P2PServiceListener#onStreamEvent} 通知。
     *
     * @param peerNodeId 对端节点标识
     * @return 流标识，0 表示失败
     * @throws IllegalStateException 已关闭
     */
    public int openStream(int peerNodeId) {
        return nativeService.openStream(requireOpen(), peerNodeId);
    }

    /**
     * @param streamId 流标识
     * @return 请求已投递返回 {@code true}
     * @throws IllegalStateException 已关闭
     */
    public boolean closeStream(int streamId) {
        return nativeService.closeStream(requireOpen(), streamId);
    }

    /**
     * 向流提交数据。
     *
     * @param streamId 流标识
     * @param data     数据内容，长度 1..{@link #MAX_PAYLOAD_SIZE}
     * @return 请求已受理返回 {@code true}
     * @throws NullPointerException     {@code data} 为 {@code null}
     * @throws IllegalArgumentException {@code data} 为空或超过 {@link #MAX_PAYLOAD_SIZE}
     * @throws IllegalStateException    已关闭
     */
    public boolean sendData(int streamId, byte[] data) {
        Objects.requireNonNull(data, "data");
        if (data.length == 0) {
            throw new IllegalArgumentException("data is empty");
        }
        if (data.length > MAX_PAYLOAD_SIZE) {
            throw new IllegalArgumentException("data exceeds MAX_PAYLOAD_SIZE: " + data.length);
        }
        return nativeService.sendData(requireOpen(), streamId, data);
    }

    /**
     * @param streamId 流标识
     * @return 流是否已打开
     * @throws IllegalStateException 已关闭
     */
    public boolean isStreamOpen(int streamId) {
        return nativeService.isStreamOpen(requireOpen(), streamId);
    }

    /**
     * 设置回调监听器，运行中替换会立即生效。
     *
     * @param listener 监听器，{@code null} 表示取消回调
     * @throws IllegalStateException 已关闭
     */
    public void setListener(P2PServiceListener listener) {
        long current = requireOpen();

        if (listener == null) {
            nativeService.setListener(current, null);
            adapter = null;
            return;
        }

        CallbackAdapter newAdapter = new CallbackAdapter(listener);
        nativeService.setListener(current, newAdapter);
        adapter = newAdapter;
    }

    /**
     * 停止服务并释放 native 句柄，幂等。
     *
     * <p>阻塞等待 native 线程退出，不要在主线程调用。
     */
    @Override
    public synchronized void close() {
        long current = handle;
        if (current == 0) {
            return;
        }

        handle = 0;
        adapter = null;

        try {
            nativeService.setListener(current, null);
        } catch (RuntimeException ignored) {
            // 清理监听器失败不应阻塞释放流程
        }

        try {
            nativeService.stop(current);
        } finally {
            nativeService.destroy(current);
        }
    }

    private long requireOpen() {
        long current = handle;
        if (current == 0) {
            throw new IllegalStateException("P2PService is closed");
        }
        return current;
    }
}
