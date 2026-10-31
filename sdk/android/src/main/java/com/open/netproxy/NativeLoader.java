package com.open.netproxy;

/**
 * 加载 native 库，包私有。
 *
 * <p>不捕获 {@link UnsatisfiedLinkError}：让错误在构造 {@link P2PService} 时暴露，
 * 异常信息包含库名，便于定位 ABI 缺失或打包遗漏。
 */
final class NativeLoader {

    /** 与 CMake 产物 libnetproxy.so 对应。 */
    private static final String LIBRARY_NAME = "netproxy";

    private static boolean loaded;

    private NativeLoader() {
    }

    static synchronized void load() {
        if (loaded) {
            return;
        }
        System.loadLibrary(LIBRARY_NAME);
        loaded = true;
    }
}
