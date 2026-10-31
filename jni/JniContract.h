#pragma once

// ---------------------------------------------------------------------------
// JNI 契约
//
// 本仓库不包含 Java 源码，SDK 侧按下面的声明实现同名类即可
// （包名/类名改动时同步修改本文件的常量与回调签名）。
//
// package com.open.netproxy;
//
// public final class P2PService implements AutoCloseable { ... }       // 对外 API
// final class P2PServiceNative {                                     // 包私有，不对外
//     interface Listener {
//         void onPeer(int nodeId, String address, int port);
//         void onStreamEvent(int streamId, int peerNodeId, int event, int reason);
//         void onData(int streamId, int peerNodeId, byte[] data);
//     }
//
//     private static native long nativeCreate();
//     private static native void nativeDestroy(long handle);
//     private static native boolean nativeStart(long handle, int nodeId, int dataPort,
//                                               String bindAddress, String multicastGroup,
//                                               int multicastPort);
//     private static native void nativeStop(long handle);
//     private static native boolean nativeIsRunning(long handle);
//     private static native int nativeNodeId(long handle);
//     private static native int nativeDataPort(long handle);
//     private static native long[] nativePeers(long handle);
//     private static native long[] nativeStreams(long handle);
//     private static native boolean nativeConnect(long handle, int peerNodeId);
//     private static native boolean nativeIsConnected(long handle, int peerNodeId);
//     private static native int nativeOpenStream(long handle, int peerNodeId);
//     private static native boolean nativeCloseStream(long handle, int streamId);
//     private static native boolean nativeSendData(long handle, int streamId, byte[] data);
//     private static native boolean nativeIsStreamOpen(long handle, int streamId);
//     private static native void nativeSetListener(long handle, Listener listener);
//     private static native void nativeSetLogcatEnabled(long handle, boolean enabled);
// }
//
// 数据约定：
// - nativePeers 返回 long[3 * N]，按 {nodeId, ipv4, port} 排列；ipv4 为 32 位无符号
//   地址（192.168.1.1 -> 0xC0A80101），由 Java 层转点分十进制。
// - nativeStreams 返回 long[3 * N]，按 {streamId, peerNodeId, 是否打开(1/0)} 排列。
// - event 与 C++ StreamEvent 一致：0 = Opened，1 = Rejected，2 = Closed。
// - nodeId / streamId 按无符号 32 位解释（Java int 保存位模式）。
// - 回调运行在服务的业务线程，禁止阻塞；禁止在回调内调用 nativeStop/nativeDestroy。
// - 只有 nativeStart 中的 6 个参数可配，其余 P2PServiceConfig 字段沿用默认值。
//
// R8/ProGuard 需要保留（写进 SDK 的 consumer-rules.pro）：
//   -keep class com.open.netproxy.P2PServiceNative { native <methods>; }
//   -keep interface com.open.netproxy.P2PServiceNative$Listener { *; }
// ---------------------------------------------------------------------------

namespace netproxy::jni {

inline constexpr char SERVICE_CLASS_NAME[] = "com/open/netproxy/P2PServiceNative";
inline constexpr char LISTENER_CLASS_NAME[] = "com/open/netproxy/P2PServiceNative$Listener";

// nativeSetListener 的 descriptor 里嵌了包名，与类名常量必须同源维护。
inline constexpr char SET_LISTENER_SIGNATURE[] = "(JLcom/open/netproxy/P2PServiceNative$Listener;)V";

inline constexpr char ON_PEER_NAME[] = "onPeer";
inline constexpr char ON_PEER_SIGNATURE[] = "(ILjava/lang/String;I)V";

inline constexpr char ON_STREAM_EVENT_NAME[] = "onStreamEvent";
inline constexpr char ON_STREAM_EVENT_SIGNATURE[] = "(IIII)V";

inline constexpr char ON_DATA_NAME[] = "onData";
inline constexpr char ON_DATA_SIGNATURE[] = "(II[B)V";

} // namespace netproxy::jni
