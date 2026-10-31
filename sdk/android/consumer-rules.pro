# JNI 依赖类名与方法名：RegisterNatives 在加载期按字符串查找，重命名会导致
# System.loadLibrary 抛 UnsatisfiedLinkError。规则随 AAR 自动合并到消费方。
-keep class com.open.netproxy.P2PServiceNative { native <methods>; }
-keep interface com.open.netproxy.P2PServiceNative$Listener { *; }
