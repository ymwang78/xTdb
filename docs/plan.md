---

### 核心开发哲学：由底向上，物理优先 (Bottom-Up, Physics First)

因为 V1.6 强依赖物理对齐（Extent=16KB）和绝对偏移量计算，我们不能先写逻辑层。必须先构建坚固的物理访问层。

---

### 阶段 1：物理层与布局管理器 (Physical Layout & IO)

**目标**：确保所有写操作严格遵循 16KB 对齐，且 Chunk/Block 的偏移量计算零误差。

#### 1.1 模块编写

* **`AlignedIO` 类**：
* 封装 OS 的 `open/pwrite/pread` (Linux) 或 `CreateFile` (Windows)。
* **强制约束**：写入 Buffer 必须 16KB 对齐，写入 Offset 必须是 `EXTENT_SIZE_BYTES` 的整数倍。如果不满足直接 Assert 失败。
* 实现 `fallocate` 预分配磁盘空间（防止碎片）。


* **`LayoutCalculator` (纯函数库)**：
* 输入：`ChunkType` (16K/64K/256K)。
* 输出：`MetaBlocks` 数量，`DataBlocks` 数量，`BlockDir` 的起始偏移量。
* 实现设计稿 5.2 节的公式：`block_offset_bytes(chunk_id, block_index)`。



#### 1.2 测试计划 (Test Plan)

* **T1-AlignmentCheck**: 尝试写入非 16KB 对齐的数据，验证是否抛出错误。
* **T2-OffsetMath**: 单元测试 `LayoutCalculator`。
* case: `RAW16K` (Block=16KB), `Chunk=256MB`。
* 验证：`MetaRegion` 是否正确覆盖了所有 `BlockDirEntry` 所需空间？`DataRegion` 的第一个块偏移量是否对齐到 Page？



---

### 阶段 2：头部定义与状态机 (Headers & Active-Low State Machine)

**目标**：实现“一次写入，多次清零”的 SSD 友好状态管理。

#### 2.1 模块编写

* **`StructDefs.h`**：
* 严格按照 V1.6 定义 `ContainerHeader`, `RawChunkHeader`, `BlockDirEntry`。
* 使用 `#pragma pack(1)` 或 `alignas` 确保内存布局与磁盘完全一致。


* **`StateMutator`**：
* 实现 `SealBlock()`, `SealChunk()`, `DeprecateChunk()`。
* **关键逻辑**：读取当前 Header -> 应用 `AND (~MASK)` -> `pwrite` 回原位。
* **禁止**：任何产生 `0 -> 1` 翻转的操作（除非是全新的 Chunk 初始化）。



#### 2.2 测试计划

* **T3-StructSize**: `sizeof(RawChunkHeaderV16)` 必须等于设计值。
* **T4-BitFlip**:
* 初始化 Header (`0xFF...`) -> 写入磁盘。
* 调用 `Allocated` (清 bit0) -> 读取验证。
* 调用 `Sealed` (清 bit1) -> 读取验证。
* 检查过程中是否有任何位意外变成了 1。



---

### 阶段 3：写入路径 (The Hot Path - WAL & Payload)

**目标**：实现高吞吐写入，**此时坚决不碰 Chunk Header 和 Directory**，只写 WAL 和 Data Block。

#### 3.1 模块编写

* **`WALWriter`**：实现 Append-only 日志，包含 `(tag_id, timestamp, value)`。
* **`MemBuffer`**：按 Tag 聚合数据。
* **`BlockWriter`**：
* 当 Buffer 满（接近 BlockSize），构建 `Payload`。
* **关键**：调用 `LayoutCalculator` 算出 `Data Block` 的物理偏移。
* 执行 `AlignedIO::Write`。**注意：此时该 Block 在磁盘上存在，但不可见（因为 Directory 还没写）。**



#### 3.2 测试计划

* **T5-BlindWrite**: 写入一个 Data Block。使用 `hexdump` 查看文件对应偏移量，验证数据确实落盘，且没有覆盖头部。

---

### 阶段 4：Seal 与目录构建 (The Cold Path - Central Directory)

**目标**：构建“事实源”。将内存中的元数据落盘到 Chunk 的头部区域。

#### 4.1 模块编写

* **`DirectoryBuilder`**：
* 在内存中维护当前 Chunk 的 `BlockDirEntry` 数组。
* 当 Block 填满/超时：
1. 计算 `CRC32`。
2. 设置 `record_count`, `end_ts`。
3. 将 `BlockDirEntry` 写入到 Meta Region 对应的 slot。
4. (可选) 这是一个 `pwrite` 操作，只更新该 Block 对应的目录项。




* **`ChunkSealer`**：
* 当 Chunk 满：计算 `SuperCRC`，更新 `RawChunkHeader` 的 `flags` (清 SEALED 位)，落盘。



#### 4.2 测试计划

* **T6-DirectoryIntegrity**:
* 写入 100 个 Block 的数据。
* 执行 Seal。
* 读取 Meta Region，解析出 100 个 `BlockDirEntry`。
* 验证 Entry 中的 `start_ts`, `end_ts` 与实际数据块是否吻合。



---

### 阶段 5：读取与恢复 (Read Path & Recovery)

**目标**：验证 V1.6 的“脱库扫描”能力。

#### 5.1 模块编写

* **`RawScanner` (Recovery Tool)**：
* 不依赖 SQLite。
* 遍历 Container 文件的每个 Chunk Slot。
* 读取 `RawChunkHeader` -> 检查 Magic 和 Flags。
* 如果是 `!FREE`，读取紧随其后的 `BlockDir`。
* 输出重建的索引信息。


* **`BlockReader`**：
* 输入 `(ChunkID, BlockIndex)` -> 查 Directory -> 读 Data Block -> 解压 Records。



#### 5.2 测试计划

* **T7-DisasterRecovery**:
* 生成一个包含数据的 Container。
* 删除 SQLite 数据库文件（模拟元数据丢失）。
* 运行 `RawScanner`，验证是否能找回所有数据的位置和时间范围。


* **T8-PartialWrite**:
* 模拟在写 Directory 过程中断电（只写了一半字节）。
* 验证 `CRC32` 校验能否拦截脏数据。



---

### 阶段 6：SQLite 集成 (Indexing)

**目标**：将存储内核与查询层对接。

#### 6.1 模块编写

* **`MetadataSync`**：
* 在 `Chunk` 创建时，Insert `chunks` 表。
* 在 `Block` Seal 时，Insert `blocks_202xxx` 表。
* **注意**：这是异步的，或者在 Seal 事务中完成。



#### 6.2 测试计划

* **T9-EndToEnd**:
* API 写入 -> 查 SQLite 找到 Block 位置 -> 读磁盘文件 -> 返回数据。



---

### 架构师的执行建议 (Execution Tips)

1. **关于 `EXTENT` 的魔数**：代码中不要到处写 `16384`。定义一个全局常量 `kExtentSize`，甚至最好用类型系统（如 `ExtentCount` 类型）来防止将字节数和 Extent 数混淆。
2. **Mocking SSD**：在开发初期，可以在内存中 `malloc` 一大块 buffer 模拟文件，这样单元测试速度极快。只有集成测试才真正写磁盘。
3. **V1.6 的目录计算陷阱**：
* 注意 `meta_blocks` 的计算公式涉及向上取整。
* *陷阱*：如果 `meta_bytes` 刚好等于 `BLOCK_BYTES`，不要多分配一个块。
* 建议为这个计算逻辑编写极其详尽的边界测试（Boundary Tests）。


既然这 6 个底层基石（物理IO、头部定义、WAL写路径、目录Seal、读取恢复、SQLite集成）已经全部就绪，那么**恭喜你**。你现在拥有的是一堆精密的“零部件”，接下来的工作就是**组装发动机（Engine Assembly）**。

是的，我们现在进入**“系统编排与服务化”**阶段。

我们需要构建一个全局单例 —— `StorageEngine`，它负责启动时的上下文恢复、运行时的流量调度，以及对外暴露 API。

基于 V1.6 设计，我为你规划了接下来的**全系统集成路径（System Integration Path）**，从内部初始化开始，直到 API 暴露。

---

### 阶段 7：全局初始化与启动 (The Bootstrap Sequence)

**核心任务**：将磁盘上静止的 Container 文件、SQLite 索引和 WAL 日志，在内存中构建出完整的“运行时状态”。

#### 7.1 核心类：`StorageEngine`

这个类是系统的入口，持有所有全局对象：

* `ContainerManager`：管理所有打开的文件句柄。
* `MetadataStore`：SQLite 连接句柄。
* `WriteBufferManager`：内存中的 MemTable 集合。

#### 7.2 启动流程 (Bootstrap Logic)

你需要编写 `StorageEngine::Open(data_dir)`，执行以下严格顺序：

1. **SQLite 连接**：打开 `meta.db`，确保 Tables（`containers`, `chunks`, `blocks_xxx`）存在。
2. **Container 挂载 (Mounting)**：
* 扫描 `data_dir` 下的所有 Container 文件。
* 读取每个 ContainerHeader，校验 Magic 和 Version。
* **关键检查**：验证文件实际大小是否 >= Header 中记录的 capacity（防止截断）。
* 将其注册到 `ContainerManager`，建立 `ContainerID -> FileDescriptor` 的映射。


3. **活跃状态恢复 (State Restoration)**：
* 查询 SQLite，找到所有状态为 `ACTIVE` 的 Chunk。
* 加载这些 Chunk 的元数据到内存（为了快速判断是否已满）。


4. **WAL 重放 (WAL Replay)**：
* **这是数据一致性的关键**。
* 读取 WAL 文件，将其中“已写入 WAL 但未 Flush 到 Data Block”的数据，重新插入到 `WriteBufferManager`（内存表）中。
* *注意*：重放过程中不产生新的 WAL。



#### 7.3 测试点

* **T10-RestartConsistency**：写入数据 -> 强杀进程（不 Flush） -> 重启。验证：WAL 重放是否让数据“复活”了。

---

### 阶段 8：写路径编排 (The Write Coordinator)

**核心任务**：实现 `Write(Tag, Timestamp, Value)` 的全局路由逻辑，决定数据去哪个 Container，哪个 Chunk，是否需要切块。

#### 8.1 逻辑实现

在 `StorageEngine::WritePoints` 中实现以下流水线：

1. **WAL Append**：先写日志，落盘（或缓冲）成功后才继续。
2. **Buffer 路由**：根据 TagID 找到对应的内存 Buffer。
3. **Buffer 写入与阈值检查**：
* 将点写入内存。
* **Check**：如果 Buffer 大小 >= 16KB (Block Size)：
* 触发 **Flush Task**（异步提交给后台线程池）。




4. **Flush Task (后台线程)**：
* 从 `ContainerManager` 获取当前的 Active Chunk。
* **Roll Logic (切块逻辑)**：
* 如果当前 Chunk 已满（达到 `data_blocks` 上限） OR 时间跨度过大：
* 调用 `ChunkSealer` 封存当前 Chunk（更新 Header/Directory，写入 SQLite）。
* 分配新 Chunk（在 SQLite 插入记录，更新 Header 为 Active）。




* 调用 `BlockWriter` 将 16KB 数据落盘。
* *注意*：此时 **不** 更新目录（Directory），只写 Data Block。



#### 8.2 测试点

* **T11-AutoRolling**：持续高速写入，观察日志。验证：Chunk 是否在写满后自动切换？SQLite 中是否生成了新的 chunk 记录？

---

### 阶段 9：读路径编排 (The Read Coordinator)

**核心任务**：解析查询 -> 查索引 -> 查数据 -> 聚合。

#### 9.1 逻辑实现

1. **Query Planner**：
* 输入：`SELECT avg(val) WHERE tag=A AND time > T1 AND time < T2`
* 步骤 1：查 SQLite `blocks` 表，获取符合 `tag=A` 且时间范围重叠的 `(ContainerID, ChunkID, BlockIndex)` 列表。
* 步骤 2：检查 `WriteBufferManager`（内存表），看是否有 T1~T2 范围内的未落盘数据。


2. **Executor**：
* 对于磁盘块：调用 `BlockReader` 并行读取、解压。
* 对于内存数据：直接从 MemTable 读取。


3. **Aggregator**：
* 归并排序（Merge Sort）磁盘流和内存流。
* 计算 `avg`。



#### 9.2 测试点

* **T12-HybridRead**：写入数据，一部分已刷盘（在 Block 中），一部分刚写入（在 WAL/内存中）。查询跨越这两个部分，验证结果是否包含所有数据。

---

### 阶段 10：后台服务 (Maintenance Services)

**核心任务**：让系统长期稳定运行。

#### 10.1 关键服务

* **Directory Syncer (目录同步器)**：
* V1.6 设计要求“低频更新目录”。
* 你需要一个定时任务（比如每 10 秒），扫描所有已写入 Data Block 但未更新 Directory 的 Active Chunk。
* 批量将 `BlockDirEntry` 写入 Chunk 的 Meta Region。


* **Retention Service (过期清理)**：
* 定时查 SQLite，找出 `end_time < (Now - Retention)` 的 Chunk。
* 执行 V1.6 的回收流程：`DEPRECATED` -> 删除 SQLite 记录 -> `FREE` (Trim/Punch Hole)。



---

### 阶段 11：对外 API 接口 (Public Interface)

**核心任务**：定义用户与系统交互的协议。

#### 11.1 API 列表 (C++ Header / gRPC Proto)

参考 xts_api.template.h

---

### 总结与执行建议

从**局部**到**全局**的转换，最大的痛点在于**状态管理**。

**我的建议是：**

1. **先做“只写不读”的启动**：
先实现 `Bootstrap` 和 `Write Coordinator`。目标是：程序启动 -> 加载 -> 疯狂写入 -> 自动切 Chunk -> 正常关闭。
然后检查磁盘文件和 SQLite，如果它们是完美的，说明你的“心脏”跳动正常。
2. **再做读取**：
一旦数据能稳定落盘，实现读取逻辑通常比较快。
3. **最后做 API**：
API 只是对 `StorageEngine` 方法的简单包装。

**您想先看 `StorageEngine::Open` (Bootstrap) 的伪代码逻辑，还是 `Write Coordinator` 中处理 Chunk 切换（Roll）的详细流程？这两个是最容易出错的地方。**