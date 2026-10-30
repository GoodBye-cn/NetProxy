# 文件传输（一条流承载多个文件）

本文描述在 `P2PService` 的连接与流之上实现的多文件传输：一对节点复用一条流，按 `fileId` 区分文件。

传输层协议见 [peer-protocol.md](peer-protocol.md)，架构总览见 [architecture.md](architecture.md)。

## 1. 目标与边界

目标：

- 一对节点复用一条流，连续传输多个文件
- 每个文件独立的状态、进度与错误，互不影响
- 单文件失败不关闭流，其它文件继续
- 流空闲一段时间后自动关闭

不做：

- 不修改传输层协议，流仍提供可靠有序
- 不做断点续传、压缩、加密（可作为后续阶段）
- 不做目录同步，只传单文件

## 2. 流复用模型

```
PeerConnection（每对节点一条）
└── 文件传输流（每个 peerNodeId 一条）
     ├── fileId=1001  报价 → 接受 → 分块 → 完成
     ├── fileId=1002  排队中
     └── fileId=1003  排队中
```

| 概念 | 标识 | 说明 |
| --- | --- | --- |
| 连接 | `peerNodeId` | 传输层已有 |
| 传输流 | `streamId` | 每个对端一条，用于文件传输 |
| 文件 | `fileId` | 流内唯一，由发起方分配 |

`FileTransfer` 维护 `peerNodeId → streamId` 映射：

- `sendFile` 时若已有流，直接复用
- 没有则 `openStream`，等 `Opened` 后补发
- 流空闲超过 `streamIdleMs` 自动 `closeStream`

## 3. 协议

### 3.1 消息头（8 字节）

| 偏移 | 长度 | 字段 | 说明 |
| --- | --- | --- | --- |
| 0 | 1 | `type` | 消息类型 |
| 1 | 1 | `flags` | 保留 |
| 2 | 2 | `reserved` | 填 0 |
| 4 | 4 | `fileId` | 文件标识，非 0 |

所有消息都带 `fileId`，一条流上多个文件靠它区分。

### 3.2 消息类型

| type | 名称 | 方向 | 负载 |
| --- | --- | --- | --- |
| 1 | `Offer` | 发送方 → 接收方 | `size(8) + nameLength(2) + name + hash(32，可选)` |
| 2 | `Accept` | 接收方 → 发送方 | 空 |
| 3 | `Reject` | 接收方 → 发送方 | `reason(1)` |
| 4 | `Chunk` | 发送方 → 接收方 | `offset(8) + data` |
| 5 | `Ack` | 接收方 → 发送方 | `receivedBytes(8)` |
| 6 | `Complete` | 发送方 → 接收方 | `size(8)` |
| 7 | `Done` | 接收方 → 发送方 | `size(8)` |
| 8 | `Cancel` | 任一方 | `reason(1)` |

单帧上限 1400，`Chunk` 的 `data` 最大 = 1400 − 8 − 8 = 1384。

### 3.3 拒绝与取消原因

| reason | 含义 |
| --- | --- |
| 1 | 磁盘空间不足 |
| 2 | 路径不可写 |
| 3 | 超过大小限制 |
| 4 | 用户拒绝 |
| 5 | 文件不存在或读取失败 |
| 6 | 内部错误 |

## 4. 状态机

### 4.1 文件状态

```
Queued --Offer发出--> Offering --Accept--> Transferring --Complete/Done--> Completed
                         |                     |                              |
                         +--Reject--> Rejected +--Cancel/超时--> Failed       +--失败--> Failed
```

| 状态 | 含义 |
| --- | --- |
| `Queued` | 等待发送（顺序模式） |
| `Offering` | 已发 Offer，等待 Accept/Reject |
| `Transferring` | 可发送 Chunk，等待 Ack 推进 |
| `Completed` | 接收方确认落盘（Done） |
| `Rejected` | 对端拒绝 |
| `Failed` | 取消、超时或流断开 |

### 4.2 流状态

- 流本身只有 `Open` / `Closed`
- 流内所有文件 `Completed` / `Rejected` / `Failed` 后，流进入空闲
- 空闲超过 `streamIdleMs` 自动关闭

## 5. 传输模式

### 5.1 模式 A：顺序复用（第一阶段）

同一时刻只有一个文件处于 `Transferring`：

```
Offer(1001) → Accept → Chunks → Complete → Done
Offer(1002) → Accept → Chunks → Complete → Done
```

- 队列按提交顺序
- 背压简单：只有当前文件有在途数据
- 缺点：大文件会阻塞后面的小文件

### 5.2 模式 B：交错复用（第二阶段）

多个文件同时 `Transferring`，按轮询或权重发送 Chunk：

```
轮次 1: 1001 的 Chunk、1002 的 Chunk
轮次 2: 1001 的 Chunk、1002 的 Chunk
...
```

- 每个文件独立 `offset` 与 `ackedBytes`
- 全局在途上限 + 每文件最小配额
- 需要调度器与公平性策略

第一阶段只做模式 A，`maxConcurrentFiles` 固定为 1；模式 B 通过配置切换。

## 6. 背压

### 6.1 流级

流层 `sendData` 只入队，不提供完成通知，所以文件层必须自己限速：

```
inflight = sentBytes - ackedBytes
inflight 达到上限 → 暂停读文件
收到 Ack → 更新 ackedBytes → 继续
```

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `chunkSize` | 1384 | 单帧可承载负载 |
| `inflightLimit` | 256KB | 全流在途上限 |
| `ackEveryBytes` | 64KB | 接收端每累计这么多字节回一次 Ack |

流层 `sendWindow` 建议调到 128 以上，否则数据堆积在 `pendingFrames_`。

### 6.2 文件级

顺序模式下，`inflightLimit` 就是当前文件的上限。

交错模式下，每个文件至少保留 `inflightLimit / maxConcurrentFiles` 的配额，避免一个文件饿死其它文件。

### 6.3 读文件

发送端按 `chunkSize` 同步读文件；单次 1.4KB 的磁盘读在业务线程可接受。

如果要做大文件，改成一次读 64KB 再切片，减少系统调用。

## 7. 错误与取消

| 场景 | 处理 |
| --- | --- |
| Offer 被拒 | 该文件 `Rejected`，流继续 |
| 发送方取消 | 发 `Cancel`，本地 `Failed`，接收方删除临时文件 |
| 接收方取消 | 发 `Cancel`，发送方停止该文件，流继续 |
| 单文件超时 | 发送方连续 `offerTimeoutMs` 未收到 Accept → `Failed` 并 `Cancel` |
| 流断开 | 流内所有未完成文件 `Failed`，回调错误 |
| 写盘失败 | 接收方回 `Cancel(2)`，删除临时文件 |

**关键原则**：单文件失败不关闭流，避免影响同流其它文件；只有流级故障（重传耗尽、心跳超时）才关闭整条流。

## 8. 落盘

- 接收方先写 `<saveDir>/<fileId>.part`
- `Complete` 到达后校验 `receivedBytes == size`，再 rename 为最终文件名
- 文件名去除路径分隔符与 `..`，防止路径穿越
- 同名文件追加序号，不覆盖
- `Cancel` 或流断开时删除 `.part`

## 9. 对外接口

```cpp
// include/FileTransfer.h
struct FileTransferConfig {
    std::string saveDirectory = "./downloads";
    std::uint64_t maxFileSize = 4ULL * 1024 * 1024 * 1024;
    std::size_t chunkSize = 1384;
    std::uint64_t inflightLimit = 256 * 1024;
    std::uint64_t ackEveryBytes = 64 * 1024;
    std::size_t maxConcurrentFiles = 1; // 1 = 顺序模式
    int offerTimeoutMs = 10000;
    int streamIdleMs = 60000;
    bool autoAccept = false;
};

class FileTransfer final {
public:
    using ProgressHandler = std::function<void(std::uint32_t fileId, std::uint32_t peerNodeId,
                                               std::uint64_t done, std::uint64_t total)>;
    using CompleteHandler = std::function<void(std::uint32_t fileId, std::uint32_t peerNodeId,
                                               const std::string& path)>;
    using ErrorHandler = std::function<void(std::uint32_t fileId, std::uint32_t peerNodeId,
                                            const std::string& reason)>;

    FileTransfer(P2PService& service, const FileTransferConfig& config);
    ~FileTransfer();

    // 发送：返回 fileId，0 表示失败（服务未启动、对端未知、文件不可读）
    std::uint32_t sendFile(std::uint32_t peerNodeId, const std::string& path);

    // 取消单个文件，不影响同流其它文件
    bool cancelFile(std::uint32_t fileId);

    // 关闭与指定对端的传输流，未完成文件全部失败
    bool closePeer(std::uint32_t peerNodeId);

    void setProgressHandler(ProgressHandler handler);
    void setCompleteHandler(CompleteHandler handler);
    void setErrorHandler(ErrorHandler handler);
};
```

事件都在业务线程触发，回调里不能阻塞。

## 10. 内部结构

```
FileTransfer
├── map<peerNodeId, PeerTransfer>
│     ├── streamId
│     ├── map<fileId, FileSession>
│     ├── queue<fileId>（顺序模式）
│     └── inflightBytes
└── 回调
```

`FileSession`：

```cpp
struct FileSession {
    std::uint32_t fileId = 0;
    std::uint32_t peerNodeId = 0;
    FileState state = FileState::Queued;
    std::uint64_t totalBytes = 0;
    std::uint64_t sentBytes = 0;
    std::uint64_t ackedBytes = 0;
    std::uint64_t receivedBytes = 0;
    std::string path;      // 发送端源文件或接收端目标文件
    std::ifstream source;  // 发送端
    std::ofstream target;  // 接收端
};
```

`FileTransfer` 通过 `P2PService` 注册：

- `setStreamHandler`：跟踪流 `Opened` / `Closed`
- `setDataHandler`：按 `streamId` 与 `fileId` 分发到 `FileSession`

注意：`P2PService` 的 handler 是单例，`FileTransfer` 需要与其它业务共用时，应在应用层做一次转发，或让 `FileTransfer` 提供 `onData` / `onStreamEvent` 供上层调用。

## 11. 配置建议

| 场景 | 调整 |
| --- | --- |
| 局域网大文件 | `sendWindow = 128`、`inflightLimit = 256KB`、`maxConcurrentFiles = 1` |
| 小文件多 | `maxConcurrentFiles = 4`、`inflightLimit = 128KB` |
| 弱网 | 保持 `retransmitTimeoutMs = 200`，降低 `inflightLimit` 到 64KB |

## 12. 分阶段实施

| 阶段 | 内容 | 验证 |
| --- | --- | --- |
| 1 | `FileMessage` 编解码 | 字段往返、非法报文 |
| 2 | 单流多文件顺序传输：Offer/Accept/Chunk/Complete/Done | 连续传 3 个文件，内容一致 |
| 3 | Ack 背压与进度回调 | 大文件内存稳定，进度单调 |
| 4 | 取消与错误隔离 | 取消中间文件，其它文件继续 |
| 5 | 流空闲关闭与复用 | 多次 `sendFile` 复用同一条流 |
| 6 | 交错模式（可选） | 多文件并发，公平调度 |

## 13. 测试计划

| 用例 | 验证 |
| --- | --- |
| 单流连续 3 个文件 | 三个文件内容一致，fileId 不串 |
| 中间文件被拒 | 第一个和第三个成功，第二个 `Rejected` |
| 单文件取消 | 发送方与接收方都清理 `.part`，流继续 |
| 1MB 多块文件 | 约 760 帧，进度回调单调 |
| 100MB 大文件 | 内存稳定，吞吐可接受 |
| 流断开 | 未完成文件全部 `Failed`，回调错误 |
| 文件名穿越 | `../etc/passwd` 被拒绝或重命名 |
| 空闲超时 | 无文件后 `streamIdleMs` 关闭流 |

## 14. 风险与边界

| 风险 | 缓解 |
| --- | --- |
| 单流单序号，顺序模式下大文件阻塞小文件 | 阶段 6 支持交错模式 |
| 业务线程同步写磁盘 | 大文件后续改为独立写线程 |
| `P2PService` handler 单例 | 应用层转发，`FileTransfer` 不直接抢占 |
| 接收方自动接受的安全风险 | 默认 `autoAccept = false`，显式确认 |
| 单文件失败影响流 | 用 `Cancel` 而非关闭流 |
| `pendingFrames_` 膨胀 | 文件层 `inflightLimit` 控制，不依赖流层背压 |
