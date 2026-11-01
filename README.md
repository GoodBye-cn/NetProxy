# NetProxy

C++17 网络代理项目，使用 CMake 构建。Linux 使用 Clang，Windows 使用 MSVC，不依赖第三方库。

文档：

- [docs/architecture.md](docs/architecture.md)：线程模型、类图、各模块接口
- [docs/discovery-protocol.md](docs/discovery-protocol.md)：组播发现协议
- [docs/peer-protocol.md](docs/peer-protocol.md)：连接与流协议
- [docs/file-transfer.md](docs/file-transfer.md)：单流多文件传输方案
- [docs/coding-style.md](docs/coding-style.md)：代码风格与注释规范

## 目录结构

| 目录 | 说明 |
| --- | --- |
| `include/` | 对外头文件，安装后直接暴露 |
| `src/` | 库源代码 |
| `src/net/` | 内部套接字与 poll 封装 |
| `src/core/` | I/O 线程与业务线程循环 |
| `src/node/` | 节点管理器、组播发现通道与节点表 |
| `src/transport/` | 连接层、流层与会话组装 |
| `src/protocol/` | 连接协议常量与类型 |
| `apps/` | 可执行程序汇总，每个子目录产出一个 Demo |
| `apps/demo/` | 交互式命令行测试程序 |
| `tests/` | 测试 |
| `docs/` | 文档 |
| `cmake/` | CMake 包配置模板 |

## 编码约定

- 文件名与主类同名，使用 `PascalCase`，如 `ProxyServer.h`、`ProxyServer.cpp`
- 命名空间使用小写 `netproxy`
- 类名 `PascalCase`，函数、方法、变量 `camelCase`，常量 `UPPER_SNAKE_CASE`
- 左大括号不换行，缩进 4 空格，行宽 120
- 头文件使用 `#pragma once`
- 风格由 `.clang-format` 固化

## 提交信息

格式由仓库根目录的 `.gitmessage` 定义，克隆后启用：

```bash
git config commit.template .gitmessage
```

- 首行 `<area>: <summary>`，`area` 取 `net` / `core` / `node` / `transport` / `apps` / `tests` / `cmake` / `docs`
- 首行 50 字内、祈使句、结尾不加句号
- 正文每行不超过 72 列，说明为什么改；无正文可只保留首行
- 尾部用 `Closes #<issue>` 关联问题，不兼容变更写 `BREAKING CHANGE:`

## 日志

默认输出到标准输出，格式为 `[时间戳] [级别] 消息`：

```cpp
#include "Log.h"

netproxy::Log::info("listening on ", port);
netproxy::Log::error("connect failed: ", reason);
```

重定向到文件或其它位置：

```cpp
netproxy::Log::setHandler([](netproxy::LogLevel level, const std::string& message) {
    // 自行格式化并写入
});
```

- `netproxy::Log::resetHandler()` 恢复标准输出
- `netproxy::Log::setMinLevel(netproxy::LogLevel::Warning)` 过滤低级别日志，默认 `Info`
- 处理器收到的 `message` 不含时间戳和级别前缀，由处理器自行决定格式
- 设置处理器与写日志都是线程安全的

## 网络封装

`src/net/` 是内部模块，不安装、不在 `include/` 中暴露，仅库内部和测试使用。用于屏蔽 Winsock 与 POSIX 的差异：

```cpp
#include "net/TCPSocket.h"

netproxy::net::TCPSocket listener;
listener.bind(netproxy::net::IPAddress::any(8080));
listener.listen();
netproxy::net::TCPSocket connection = listener.accept();
```

- `IPAddress`：地址值类型，封装 `sockaddr`，`parse` / `loopback` / `any` 构造，`host` / `port` / `toString` 读取
- `Socket`：句柄 RAII、有效性、关闭、错误信息、本地端口、地址复用
- `TCPSocket`：`connect` / `bind` / `listen` / `accept` / `send` / `receive` / `shutdownWrite` / `setNoDelay` / `setNonBlocking`
- `UDPSocket`：`bind` / `sendTo` / `receiveFrom` / `setBroadcast`，组播支持 `joinMulticastGroup` / `leaveMulticastGroup` / `setMulticastLoop` / `setMulticastTtl`
- `Poller`：`poll` / `WSAPoll` 封装，支持跨线程 `add` / `modify` / `remove` / `wakeup`；每次 `poll` 取当前注册表快照，唤醒通道固定在 `pollfd` 首位
- `EventDispatcher`：在 `Poller` 之上按句柄分发事件回调，回调在锁外执行，可在回调内增删注册
- 操作失败返回 `false` 或 `-1`，错误详情用 `lastErrorCode()` 和 `lastErrorMessage()` 获取
- Windows 首次创建套接字时自动执行 `WSAStartup`，无需调用方关心

## 节点发现通道

`src/node/NodeDiscovery` 是内部组件，只负责组播公告的收发与解析：绑定组播端口、发送 Announce/Bye、把收到的报文解析成 `PeerInfo` 后回调。报文格式见 [docs/discovery-protocol.md](docs/discovery-protocol.md)。

```cpp
#include "core/IoLoop.h"
#include "node/NodeDiscovery.h"

netproxy::core::IoLoop io;
netproxy::node::NodeDiscovery discovery(io, 0x12345678, "239.255.42.99", 45454);
discovery.open(7001);                     // 公告数据端口
discovery.setReceiver([](const netproxy::node::PeerInfo& peer) {
    // 运行在 I/O 线程，不要阻塞
});
io.start();

discovery.announce();                     // 周期调用，建议 3 秒一次
```

- 组播地址默认 `239.255.42.99:45454`，TTL 1，只在本网段传播
- 报文固定 16 字节：magic + version + type + address + port + nodeId
- 不持有定时器，也不写节点表；公告周期与状态管理由 `PeerDirectory` 负责

## P2P 服务

`include/P2PService.h` 是对外入口，内部封装了组播发现、节点表、连接与流、以及两个工作线程，外部只需包含这一个头文件。

```cpp
#include "P2PService.h"

netproxy::P2PServiceConfig config;
config.nodeId = 0;        // 0 表示随机生成
config.dataPort = 0;      // 0 表示系统分配

netproxy::P2PService service;
service.setPeerHandler([](const netproxy::PeerInfo& peer) {
    // 发现新节点，运行在业务线程
});
service.setStreamHandler([](std::uint32_t streamId, std::uint32_t peerNodeId,
                            netproxy::StreamEvent event, std::uint8_t reason) {
    // Opened / Rejected / Closed
});
service.setDataHandler([](std::uint32_t streamId, std::uint32_t peerNodeId,
                          const char* data, std::size_t size) {
    // 收到数据，运行在业务线程
});

if (!service.start(config)) {
    return 1;
}

const std::uint32_t streamId = service.openStream(peerNodeId);  // 0 表示失败
service.sendData(streamId, data, size);
service.closeStream(streamId);

service.stop();
```

- 配置覆盖节点标识、数据端口、绑定地址、组播组与端口、公告周期、流上限与连接重试参数
- `openStream` 自动确保连接：未连接时先发 Hello，连接就绪后补发 StreamOpen
- 返回非 0 不代表对端可达，超限或拒绝都会回调 `Rejected`
- `StreamHandler` 参数为 `(streamId, peerNodeId, event, reason)`，在业务线程执行
- 回调运行在业务线程，不能执行阻塞操作，也不要调用 `stop()`

### 测试程序

`netproxy_demo` 启动后进入交互模式，可以手动开流与发送数据：

```bash
# Linux
./build/linux-clang/bin/netproxy_demo --node-id 1 --port 7001

# Windows
.\build\windows-msvc\bin\netproxy_demo.exe --node-id 2 --port 7002
```

两个平台的可执行文件都输出到构建目录的 `bin/` 下。

| 命令 | 说明 |
| --- | --- |
| `open <nodeId>` | 向已发现的节点开流 |
| `close <streamId>` | 关闭流 |
| `send <streamId> <text>` | 向流发送一条数据 |
| `streams` | 列出当前流 |
| `peers` | 列出已发现节点 |
| `quit` | 退出 |

两个终端分别启动 `--node-id 1` 与 `--node-id 2`，在其中一个执行 `open 2`，双方日志会打印 `流已打开`。

## 节点管理器

`src/node/PeerDirectory` 运行在业务线程，维护 `PeerTable`、驱动周期公告与过期淘汰。socket 由 `NodeDiscovery` 持有，节点管理器不再自己创建。

```cpp
#include "core/IoLoop.h"
#include "core/TaskLoop.h"
#include "node/NodeDiscovery.h"
#include "node/PeerDirectory.h"

netproxy::core::IoLoop io;
netproxy::core::TaskLoop business;

netproxy::node::NodeDiscovery discovery(io, 0x12345678, "239.255.42.99", 45454);
discovery.open(7001);

netproxy::node::DiscoveryConfig config;
config.announceIntervalMs = 3000;
config.peerTimeoutMs = 10000;

netproxy::node::PeerDirectory manager(discovery, 0x12345678, config, io, business);
manager.setPeerHandler([](const netproxy::node::PeerInfo& peer) { /* 新节点 */ });
manager.start();                      // 设置通道回调、安排定时
io.start();
business.start();

netproxy::node::PeerInfo peer;
if (manager.findPeer(targetId, peer)) {
    // 用 peer.address:peer.port 建立会话
}

business.stop();
io.stop();
manager.stop();
discovery.close();
```

- `start()` 只设置 `NodeDiscovery` 的回调并投递第一个 tick，不创建任何 socket
- 公告与淘汰在业务线程执行，发送通过 `IoLoop::post()` 回到 I/O 线程
- 停止顺序：业务线程 → I/O 线程 → `PeerDirectory` → `NodeDiscovery`

## 连接与流

`src/transport/` 负责连接与流，内部分两层：`connection/` 管握手、序号、窗口与重传，`stream/` 管流表与流信令。每个节点对由 `PeerSession` 把两者组合起来，上层业务直接在连接上开流。

```cpp
#include "transport/DataChannel.h"
#include "transport/PeerSessionManager.h"
#include "transport/connection/PeerConfig.h"
#include "transport/stream/StreamConfig.h"

netproxy::transport::DataChannel channel(io);
channel.open("0.0.0.0", 7001);

netproxy::transport::PeerConfig peerConfig;
netproxy::transport::StreamConfig streamConfig;
streamConfig.maxStreams = 16;
netproxy::transport::PeerSessionManager connections(channel, 0x12345678, peerConfig, streamConfig, business);
connections.start();

netproxy::PeerInfo peer;
if (!node.findPeer(peerNodeId, peer)) {
    return;
}

const std::uint32_t streamId =
    connections.openStream(peerNodeId, netproxy::net::IPAddress::parse(peer.address, peer.port));
connections.sendData(streamId, data, size);
connections.closeStream(streamId);
```

- 握手：`Hello` / `HelloAck` / `HelloReject`，裸 UDP 重发，协议版本 2
- 连接：连接级序号、累计 ACK、窗口、重传，所有帧（含 `StreamClose`）都走可靠层
- 流：`StreamOpen` / `StreamAccept` / `StreamReject` / `StreamClose`，`streamId` 为连接内唯一标识
- 心跳：连接级 `Ping`，空闲 30 秒发送，连续 3 次失败判定断链
- 事件回调 `Opened` / `Rejected` / `Closed` 在业务线程执行
- 协议细节见 [docs/peer-protocol.md](docs/peer-protocol.md)

## 构建

### Linux

```bash
cmake --preset linux-clang
cmake --build --preset linux-clang -j
ctest --preset linux-clang
```

Release 配置使用 `linux-clang-release` 预设。构建目录为 `build/linux-clang`。

测试默认只在失败时输出信息，可在命令行打开更详细的内容：

- `--verbose` / `-v`：打印每个用例的执行状态
- `--log-debug`：把日志级别调到 `Debug`，看到模块内部轨迹

```bash
./build/linux-clang/bin/netproxy_tests --verbose --log-debug
```

### Windows

```bash
cmake --preset windows-vs2026
cmake --build --preset windows-vs2026
ctest --preset windows-vs2026
```

Release 配置使用 `windows-vs2026-release` 预设，构建目录为 `build/windows-vs2026`。

Visual Studio 2022 环境改用 `windows-msvc` / `windows-msvc-release` 预设，构建目录为 `build/windows-msvc`。

预设文件本身要求 CMake 3.21 或更高版本；其中 `windows-vs2026` 使用 Visual Studio 18 2026 生成器，需要 CMake 4.2 及以上，且仅在 Windows 上可见。不使用预设时等价命令：

```bash
# Linux
cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure

# Windows（Visual Studio 2022 换成 "Visual Studio 17 2022"）
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

## 选项

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `NETPROXY_BUILD_APPS` | `ON` | 构建 `apps/` 下的程序 |
| `NETPROXY_BUILD_TESTS` | `ON` | 构建 `tests/` 下的测试 |
| `BUILD_SHARED_LIBS` | `OFF` | 设为 `ON` 时构建动态库 |
