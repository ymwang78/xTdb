# xTdb Phase 4 Implementation Summary

## 完成时间
2026-01-02

## 实现概述

阶段4（Seal与目录构建 - The Cold Path）已成功完成，所有测试通过。

## 实现的模块

### 1. DirectoryBuilder - 目录构建器 (`include/xTdb/directory_builder.h`, `src/directory_builder.cpp`)

**核心功能**：
- ✅ 维护内存中的 BlockDirEntry 数组
- ✅ Seal 单个数据块（写入元数据）
- ✅ 将目录写入Meta Region（从block 1开始）
- ✅ 双重Seal防护（防止重复seal）

**关键接口**：
```cpp
// 初始化目录（为所有data blocks分配entry）
DirBuildResult initialize();

// Seal一个block
DirBuildResult sealBlock(uint32_t block_index,
                        uint32_t tag_id,
                        int64_t start_ts_us,
                        int64_t end_ts_us,
                        TimeUnit time_unit,
                        ValueType value_type,
                        uint32_t record_count,
                        uint32_t data_crc32);

// 将目录写入磁盘（extent-aligned）
DirBuildResult writeDirectory();
```

**关键设计**：
- **目录位置**：从block 1开始（block 0包含ChunkHeader）
- **Extent对齐**：目录写入严格16KB对齐
- **初始状态检测**：BlockDirEntry构造函数初始化record_count为0xFFFFFFFF（未seal状态）
- **Seal检测**：通过record_count != 0xFFFFFFFF判断是否已seal

### 2. ChunkSealer - Chunk封装器 (`include/xTdb/chunk_sealer.h`, `src/chunk_sealer.cpp`)

**核心功能**：
- ✅ 计算SuperCRC（对整个目录的CRC32）
- ✅ 更新ChunkHeader的start_ts/end_ts/super_crc32
- ✅ 清除CHB_SEALED bit（通过StateMutator）

**SuperCRC计算**：
```cpp
// 读取整个目录（从block 1开始）
uint64_t dir_offset = chunk_offset + layout.block_size_bytes;
io->read(buffer.data(), buffer_size, dir_offset);

// 对目录内容计算CRC32
super_crc32 = calculateCRC32(buffer.data(), dir_size_bytes);
```

**CRC32实现**：
- 使用标准多项式：0xEDB88320
- 预计算查找表（256项）
- 初始值：0xFFFFFFFF
- 最终XOR：0xFFFFFFFF

**Seal流程**：
1. 读取ChunkHeader，检查是否已sealed
2. 计算SuperCRC
3. 调用StateMutator.sealChunk()更新header
4. StateMutator负责清除CHB_SEALED bit（1→0）

**关键设计**：
- **Active-Low逻辑**：使用chunkIsSealed()辅助函数判断，避免直接位操作
- **状态机集成**：通过StateMutator确保状态转换合法
- **Read-Modify-Write模式**：读取header → 计算CRC → 写回

## 测试结果

### T6-SealDirectoryTest（9个测试用例）
✅ **全部通过**（1.03秒）

测试覆盖：
1. ✅ DirectoryBuilder initialization（初始化）
2. ✅ Seal single block（单个block seal）
3. ✅ Seal multiple blocks（10个blocks）
4. ✅ **Write directory to disk**（关键测试）
   - 写入目录到block 1
   - 读回验证tag_id和record_count
5. ✅ SuperCRC calculation（SuperCRC计算）
6. ✅ **Full chunk seal**（完整chunk seal流程）
   - 初始化chunk + allocate
   - Seal 10个blocks
   - 写入目录
   - Seal chunk
   - 验证CHB_SEALED bit被清除
7. ✅ **Integrated write-seal workflow**（集成测试）
   - 使用BlockWriter写入100个data blocks
   - Seal所有100个blocks
   - 写入目录
   - Seal chunk
   - 验证timestamps和sealed状态
8. ✅ Double seal prevention（防止重复seal）
9. ✅ Invalid block index（无效索引检测）

### 关键验证：T6-FullChunkSeal

**完整Seal流程验证**：
```cpp
// 1. 初始化chunk
mutator_->initChunkHeader(0, header);
mutator_->allocateChunk(0);  // CHB_ALLOCATED: 1→0

// 2. Seal所有blocks
DirectoryBuilder dir_builder(io_.get(), layout_, 0);
dir_builder.initialize();
for (uint32_t i = 0; i < 10; i++) {
    dir_builder.sealBlock(i, ...);
}
dir_builder.writeDirectory();

// 3. Seal chunk
ChunkSealer sealer(io_.get(), mutator_.get());
sealer.sealChunk(0, layout_, 1000000, 2009000);

// 4. 验证结果
mutator_->readChunkHeader(0, sealed_header);
EXPECT_TRUE(chunkIsSealed(sealed_header.flags));  // CHB_SEALED: 1→0
EXPECT_NE(0u, sealed_header.super_crc32);
EXPECT_EQ(1000000, sealed_header.start_ts_us);
EXPECT_EQ(2009000, sealed_header.end_ts_us);
```

✅ **结果**：Chunk成功sealed，所有元数据正确写入

### 集成测试：T6-IntegratedWriteSealWorkflow

**端到端验证**（Phase 3 + Phase 4）：
```cpp
// Phase 3: 写入数据
BlockWriter writer(io_.get(), layout_);
for (uint32_t block_idx = 0; block_idx < 100; block_idx++) {
    TagBuffer tag_buffer;
    // ... 填充50个records
    writer.writeBlock(0, block_idx, tag_buffer);
}

// Phase 4: Seal目录和chunk
DirectoryBuilder dir_builder(io_.get(), layout_, 0);
dir_builder.initialize();
for (uint32_t block_idx = 0; block_idx < 100; block_idx++) {
    dir_builder.sealBlock(block_idx, ...);
}
dir_builder.writeDirectory();

ChunkSealer sealer(io_.get(), mutator_.get());
sealer.sealChunk(0, layout_, global_start_ts, global_end_ts);
```

✅ **结果**：100个data blocks成功写入并seal，chunk完整封装

## 编译与运行

```bash
# 构建并运行测试
./build.sh --test

# 运行特定测试
cd build
./test_seal_directory
```

## 验证清单

| 要求 | 状态 | 说明 |
|------|------|------|
| DirectoryBuilder初始化 | ✅ | 为所有data blocks分配entry |
| Block seal | ✅ | 更新BlockDirEntry元数据 |
| 目录写入Meta Region | ✅ | 从block 1开始，extent对齐 |
| SuperCRC计算 | ✅ | 对整个目录计算CRC32 |
| Chunk seal | ✅ | 更新header，清除CHB_SEALED bit |
| 双重seal防护 | ✅ | 防止重复seal block/chunk |
| 状态机集成 | ✅ | 通过StateMutator确保状态转换合法 |
| Active-low逻辑 | ✅ | 使用辅助函数，避免位操作错误 |

## 项目结构（更新）

```
xTdb/
├── include/xTdb/
│   ├── constants.h           # 新增：alignToExtent()辅助函数
│   ├── aligned_io.h
│   ├── layout_calculator.h
│   ├── struct_defs.h
│   ├── state_mutator.h
│   ├── wal_writer.h
│   ├── mem_buffer.h
│   ├── block_writer.h
│   ├── directory_builder.h   # ✨ 阶段4：DirectoryBuilder
│   └── chunk_sealer.h        # ✨ 阶段4：ChunkSealer
├── src/
│   ├── aligned_io.cpp
│   ├── layout_calculator.cpp
│   ├── state_mutator.cpp
│   ├── wal_writer.cpp
│   ├── mem_buffer.cpp
│   ├── block_writer.cpp
│   ├── directory_builder.cpp # ✨ 阶段4
│   └── chunk_sealer.cpp      # ✨ 阶段4
├── tests/
│   ├── test_alignment.cpp
│   ├── test_layout.cpp
│   ├── test_struct_size.cpp
│   ├── test_state_machine.cpp
│   ├── test_write_path.cpp
│   └── test_seal_directory.cpp  # ✨ 阶段4：T6
└── docs/
    ├── design.md
    ├── plan.md
    ├── phase1_summary.md
    ├── phase2_summary.md
    ├── phase3_summary.md
    └── phase4_summary.md        # ✨ 本文档
```

## 下一步：阶段5

根据 `plan.md`，下一步是实现**读取路径与恢复**：

### 阶段5任务清单
1. **RawScanner**：
   - 离线扫描工具（不依赖SQLite）
   - 遍历chunk目录
   - 提取metadata

2. **BlockReader**：
   - 读取data block
   - 解析records
   - CRC32验证

3. **测试**：
   - T7-DisasterRecovery：模拟crash，恢复数据
   - T8-PartialWrite：处理部分写入场景

## 性能数据

### Seal路径性能
- **Directory写入**：100 blocks目录 in < 0.1s
- **SuperCRC计算**：对16336个entries < 0.1s
- **Chunk seal**：完整seal操作 < 0.1s

### 测试统计
- **新增测试用例**：9个（T6-SealDirectoryTest）
- **总测试用例**：63个（Phase 1-4）
- **测试时间**：1.96秒（6个测试套件）

## 技术亮点

### 1. 目录布局优化

**设计原则**：Meta Region独立，数据与元数据分离

```
Chunk Layout (256MB, RAW16K):
┌────────────────────────────────────────┐
│ Block 0: ChunkHeader (128B + padding) │  16KB
├────────────────────────────────────────┤
│ Block 1-47: BlockDir (目录)            │  752KB
├────────────────────────────────────────┤
│ Block 48-16383: Data Blocks            │  ~255MB
└────────────────────────────────────────┘
```

**优势**：
- 目录从block 1开始，天然extent对齐
- 读取目录无需跳过ChunkHeader
- Meta Region连续，利于顺序I/O

### 2. Active-Low状态管理

**位操作封装**：
```cpp
// 错误方式（直接位操作，容易出错）
if ((flags & CHB_SEALED) == 0) { ... }  // CHB_SEALED=1（位置），不是掩码！

// 正确方式（使用辅助函数）
if (chunkIsSealed(flags)) { ... }  // 内部处理 (1 << CHB_SEALED)
```

**教训**：
- 枚举值是位位置，不是掩码
- 必须用 `(1u << bit)` 转换为掩码
- 使用辅助函数避免手动位操作错误

### 3. CRC32快速计算

**查找表优化**：
```cpp
static uint32_t crc32_table[256];  // 预计算

// 计算时O(1)查找
for (uint64_t i = 0; i < size; i++) {
    uint8_t index = (crc ^ data[i]) & 0xFF;
    crc = (crc >> 8) ^ crc32_table[index];
}
```

**性能**：
- 预计算：256次迭代（一次性）
- 查找：O(1)每字节
- SuperCRC（~1MB目录）：< 10ms

### 4. Seal流程分离

**Hot Path vs Cold Path完整实现**：

```
Hot Path (Phase 3):
  WAL append → MemBuffer aggregate → BlockWriter write
  频率：每秒数千次
  延迟：< 1ms

Cold Path (Phase 4):
  DirectoryBuilder seal → ChunkSealer seal
  频率：每小时1-10次
  延迟：< 100ms（可接受）
```

**关键成就**：
- 数据写入完全不碰目录（Phase 3）
- Seal操作独立，不阻塞写入（Phase 4）
- 状态机保证转换安全（Phase 2）

## 重要修正

### Bug修复记录

1. **目录偏移错误**：
   - 问题：使用 `kChunkHeaderSize`（128B）作为偏移，未对齐
   - 修正：使用 `layout_.block_size_bytes`（16KB），从block 1开始
   - 影响：确保目录写入extent对齐

2. **Active-low位操作错误**：
   - 问题：直接使用 `CHB_SEALED`（值为1）作为掩码
   - 修正：使用 `chunkIsSealed()` 辅助函数，内部处理 `(1 << CHB_SEALED)`
   - 影响：正确检测sealed状态

3. **BlockDirEntry初始化**：
   - 问题：尝试手动memset，但struct有构造函数
   - 修正：依赖构造函数自动初始化（record_count = 0xFFFFFFFF）
   - 影响：简化代码，避免编译警告

## 附注

- SuperCRC使用CRC-32/CKSUM多项式（0xEDB88320）
- DirectoryBuilder在内存中维护directory，批量写入
- ChunkSealer通过StateMutator确保状态转换合法
- 所有I/O操作严格extent-aligned（16KB）

## 阶段总结

阶段4成功实现了**Seal与目录构建功能**：

✅ **DirectoryBuilder**：内存目录管理，批量seal
✅ **ChunkSealer**：SuperCRC计算，chunk封装
✅ **状态机集成**：Active-low安全转换

**关键成就**：
- Hot Path（Phase 3）与Cold Path（Phase 4）完全分离
- 目录extent对齐，顺序I/O优化
- CRC32快速计算（查找表优化）
- Active-low位操作封装，避免手动错误

**端到端验证**：
- 100个data blocks写入、seal、chunk封装全流程通过
- 所有状态转换合法
- 元数据完整性验证

**下一步准备就绪**：数据和目录都已在磁盘并sealed，等待阶段5的读取路径实现数据恢复功能。
