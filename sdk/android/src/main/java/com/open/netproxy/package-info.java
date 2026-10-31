/**
 * NetProxy Android SDK 的公开 API。
 *
 * <p>对外只暴露 {@link com.open.netproxy.P2PService}、{@link com.open.netproxy.P2PServiceConfig}、
 * {@link com.open.netproxy.P2PServiceListener} 与数据模型；JNI 绑定类
 * {@code P2PServiceNative} 为包私有，不属于公开 API。
 *
 * <p>典型用法：
 *
 * <pre>{@code
 * P2PServiceConfig config = P2PServiceConfig.builder()
 *         .setDataPort(0)
 *         .build();
 *
 * try (P2PService service = new P2PService(listener)) {
 *     service.start(config);
 *     int streamId = service.openStream(peerNodeId);
 *     service.sendData(streamId, payload);
 * }
 * }</pre>
 *
 * <p>注意：{@code close()} 会阻塞等待 native 线程退出，不要在主线程调用；
 * 回调运行在 native 业务线程，禁止阻塞或在回调内关闭实例。
 */
package com.open.netproxy;
