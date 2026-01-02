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



**您想从哪里开始？**
我们可以先实现 **Phase 1 (AlignedIO & LayoutCalculator)**，因为这是整个 V1.6 设计的物理基石。如果您同意，我可以为您生成这部分的 C++ (或 Go/Rust) 代码骨架。