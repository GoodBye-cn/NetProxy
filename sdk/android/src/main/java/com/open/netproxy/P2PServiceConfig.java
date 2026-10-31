package com.open.netproxy;

/**
 * {@link P2PService} 的启动配置，不可变。
 *
 * <p>只映射 native 侧当前开放的 6 个参数；窗口、超时、心跳等字段沿用 C++ 默认值，
 * 后续版本通过新增重载方法开放。
 */
public final class P2PServiceConfig {

    /** 默认组播组。 */
    public static final String DEFAULT_MULTICAST_GROUP = "239.255.42.99";

    /** 默认组播端口。 */
    public static final int DEFAULT_MULTICAST_PORT = 45454;

    private final int nodeId;
    private final int dataPort;
    private final String bindAddress;
    private final String multicastGroup;
    private final int multicastPort;
    private final boolean logcatEnabled;

    private P2PServiceConfig(Builder builder) {
        this.nodeId = builder.nodeId;
        this.dataPort = builder.dataPort;
        this.bindAddress = builder.bindAddress;
        this.multicastGroup = builder.multicastGroup;
        this.multicastPort = builder.multicastPort;
        this.logcatEnabled = builder.logcatEnabled;
    }

    /** @return 新建构造器 */
    public static Builder builder() {
        return new Builder();
    }

    /** @return 节点标识，0 表示启动时随机生成 */
    public int getNodeId() {
        return nodeId;
    }

    /** @return 数据端口，0 表示由系统分配 */
    public int getDataPort() {
        return dataPort;
    }

    /** @return 数据 socket 绑定地址，空串表示 0.0.0.0 */
    public String getBindAddress() {
        return bindAddress;
    }

    /** @return 发现通道组播组 */
    public String getMulticastGroup() {
        return multicastGroup;
    }

    /** @return 发现通道组播端口 */
    public int getMulticastPort() {
        return multicastPort;
    }

    /** @return 是否把 native 日志桥接到 logcat，默认 {@code true} */
    public boolean isLogcatEnabled() {
        return logcatEnabled;
    }

    /** 配置构造器。 */
    public static final class Builder {

        private int nodeId;
        private int dataPort;
        private String bindAddress = "";
        private String multicastGroup = DEFAULT_MULTICAST_GROUP;
        private int multicastPort = DEFAULT_MULTICAST_PORT;
        private boolean logcatEnabled = true;

        private Builder() {
        }

        /**
         * @param nodeId 节点标识，0 表示随机生成；按无符号 32 位解释
         * @return this
         */
        public Builder setNodeId(int nodeId) {
            this.nodeId = nodeId;
            return this;
        }

        /**
         * @param dataPort 数据端口，0 表示系统分配
         * @return this
         */
        public Builder setDataPort(int dataPort) {
            this.dataPort = dataPort;
            return this;
        }

        /**
         * @param bindAddress 绑定地址，{@code null} 或空串表示 0.0.0.0
         * @return this
         */
        public Builder setBindAddress(String bindAddress) {
            this.bindAddress = bindAddress == null ? "" : bindAddress;
            return this;
        }

        /**
         * @param multicastGroup 组播组地址，不能为空
         * @return this
         */
        public Builder setMulticastGroup(String multicastGroup) {
            this.multicastGroup = multicastGroup;
            return this;
        }

        /**
         * @param multicastPort 组播端口，1..65535
         * @return this
         */
        public Builder setMulticastPort(int multicastPort) {
            this.multicastPort = multicastPort;
            return this;
        }

        /**
         * @param logcatEnabled 是否把 native 日志桥接到 logcat，默认开启
         * @return this
         */
        public Builder setLogcatEnabled(boolean logcatEnabled) {
            this.logcatEnabled = logcatEnabled;
            return this;
        }

        /**
         * @return 不可变配置
         * @throws IllegalArgumentException 端口越界或组播组为空
         */
        public P2PServiceConfig build() {
            if (dataPort < 0 || dataPort > 65535) {
                throw new IllegalArgumentException("dataPort out of range: " + dataPort);
            }
            if (multicastPort < 1 || multicastPort > 65535) {
                throw new IllegalArgumentException("multicastPort out of range: " + multicastPort);
            }
            if (multicastGroup == null || multicastGroup.isEmpty()) {
                throw new IllegalArgumentException("multicastGroup is empty");
            }
            return new P2PServiceConfig(this);
        }
    }
}
