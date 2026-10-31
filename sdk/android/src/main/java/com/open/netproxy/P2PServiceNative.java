package com.open.netproxy;

/**
 * JNI 绑定，包私有，不对外暴露。
 *
 * <p>类名、方法名、签名与 {@code jni/JniContract.h} 中的契约严格一致，
 * 任何改名都会导致 {@code RegisterNatives} 失败，进而使 {@code System.loadLibrary} 抛
 * {@link UnsatisfiedLinkError}。R8 规则见 {@code consumer-rules.pro}。
 *
 * <p>所有方法以 {@code long handle} 为第一个参数，由 {@link P2PService} 持有并在关闭后置 0。
 */
final class P2PServiceNative {

    /** native 原始回调接口，包私有；业务语义由 {@link CallbackAdapter} 转换。 */
    interface Listener {

        void onPeer(int nodeId, String address, int port);

        void onStreamEvent(int streamId, int peerNodeId, int event, int reason);

        void onData(int streamId, int peerNodeId, byte[] data);
    }

    static {
        NativeLoader.load();
    }

    long create() {
        return nativeCreate();
    }

    void destroy(long handle) {
        nativeDestroy(handle);
    }

    boolean start(long handle, int nodeId, int dataPort, String bindAddress, String multicastGroup,
                  int multicastPort) {
        return nativeStart(handle, nodeId, dataPort, bindAddress, multicastGroup, multicastPort);
    }

    void stop(long handle) {
        nativeStop(handle);
    }

    boolean isRunning(long handle) {
        return nativeIsRunning(handle);
    }

    int nodeId(long handle) {
        return nativeNodeId(handle);
    }

    int dataPort(long handle) {
        return nativeDataPort(handle);
    }

    long[] peers(long handle) {
        return nativePeers(handle);
    }

    long[] streams(long handle) {
        return nativeStreams(handle);
    }

    boolean connect(long handle, int peerNodeId) {
        return nativeConnect(handle, peerNodeId);
    }

    boolean isConnected(long handle, int peerNodeId) {
        return nativeIsConnected(handle, peerNodeId);
    }

    int openStream(long handle, int peerNodeId) {
        return nativeOpenStream(handle, peerNodeId);
    }

    boolean closeStream(long handle, int streamId) {
        return nativeCloseStream(handle, streamId);
    }

    boolean sendData(long handle, int streamId, byte[] data) {
        return nativeSendData(handle, streamId, data);
    }

    boolean isStreamOpen(long handle, int streamId) {
        return nativeIsStreamOpen(handle, streamId);
    }

    void setListener(long handle, Listener listener) {
        nativeSetListener(handle, listener);
    }

    void setLogcatEnabled(long handle, boolean enabled) {
        nativeSetLogcatEnabled(handle, enabled);
    }

    private static native long nativeCreate();

    private static native void nativeDestroy(long handle);

    private static native boolean nativeStart(long handle, int nodeId, int dataPort, String bindAddress,
                                              String multicastGroup, int multicastPort);

    private static native void nativeStop(long handle);

    private static native boolean nativeIsRunning(long handle);

    private static native int nativeNodeId(long handle);

    private static native int nativeDataPort(long handle);

    private static native long[] nativePeers(long handle);

    private static native long[] nativeStreams(long handle);

    private static native boolean nativeConnect(long handle, int peerNodeId);

    private static native boolean nativeIsConnected(long handle, int peerNodeId);

    private static native int nativeOpenStream(long handle, int peerNodeId);

    private static native boolean nativeCloseStream(long handle, int streamId);

    private static native boolean nativeSendData(long handle, int streamId, byte[] data);

    private static native boolean nativeIsStreamOpen(long handle, int streamId);

    private static native void nativeSetListener(long handle, Listener listener);

    private static native void nativeSetLogcatEnabled(long handle, boolean enabled);
}
