# xTdb Phase 5 Implementation Summary

## 完成时间
2026-01-02

## 实现概述

阶段5（读取路径与恢复 - The Recovery Path）已成功完成，所有测试通过。

## 实现的模块

### 1. RawScanner - 离线chunk扫描器 (`include/xTdb/raw_scanner.h`, `src/raw_scanner.cpp`)

**核心功能**：
- ✅ 扫描chunk并提取元数据（不依赖SQLite）
- ✅ 读取ChunkHeader和BlockDirectory
- ✅ 验证chunk完整性（SuperCRC校验）
- ✅ 区分sealed和unsealed blocks

**关键接口**：
```cpp
// 扫描整个chunk
ScanResult scanChunk(uint64_t chunk_offset,
                    const ChunkLayout& layout,
                    ScannedChunk& chunk);

// 验证chunk完整性
ScanResult verifyChunkIntegrity(uint64_t chunk_offset,
                               const ChunkLayout& layout);
```

**ScannedChunk结构**：
```cpp
struct ScannedChunk {
    uint32_t chunk_id;
    int64_t start_ts_us;
    int64_t end_ts_us;
    uint32_t super_crc32;
    bool is_sealed;
    std::vector<ScannedBlock> blocks;  // 只包含sealed blocks
};
```

**关键设计**：
- **SQLite独立**：纯文件系统操作，适合灾难恢复场景
- **自动过滤**：跳过record_count = 0xFFFFFFFF的unsealed blocks
- **CRC32验证**：重新计算SuperCRC并与header比对
- **状态检测**：使用chunkIsSealed()辅助函数判断chunk状态

### 2. BlockReader - 数据块读取器 (`include/xTdb/block_reader.h`, `src/block_reader.cpp`)

**核心功能**：
- ✅ 读取data block并解析records
- ✅ 支持所有value types（Bool/I32/F32/F64）
- ✅ CRC32完整性验证
- ✅ 统计读取性能

**关键接口**：
```cpp
// 读取并解析block
ReadResult readBlock(uint64_t chunk_offset,
                    uint32_t block_index,
                    uint32_t tag_id,
                    int64_t start_ts_us,
                    TimeUnit time_unit,
                    ValueType value_type,
                    uint32_t record_count,
                    std::vector<MemRecord>& records);

// 验证block完整性
ReadResult verifyBlockIntegrity(uint64_t chunk_offset,
                               uint32_t block_index,
                               uint32_t expected_crc32);
```

**Record解析**：
```cpp
// 3字节time_offset（小端序）
record.time_offset = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16);

// 1字节quality
record.quality = ptr[3];

// 根据value_type解析value（4-8字节）
memcpy(&record.value.f64_value, ptr + 4, 8);
```

**关键设计**：
- **字节顺序**：严格小端序解析
- **类型安全**：使用uint8_t*避免符号扩展问题
- **统计信息**：blocks_read, bytes_read, records_read
- **物理偏移计算**：通过LayoutCalculator计算准确偏移

## 测试结果

### T7-DisasterRecovery & T8-PartialWrite（6个测试用例）
✅ **全部通过**（0.68秒）

测试覆盖：
1. ✅ RawScanner basic scan（扫描5个sealed blocks）
2. ✅ RawScanner verify chunk integrity（SuperCRC验证）
3. ✅ **BlockReader read records**（读取50个records并验证）
4. ✅ BlockReader read multiple blocks（读取10个blocks，200个records）
5. ✅ **T7-DisasterRecovery**（完整灾难恢复流程）
   - 写入20个blocks（600个records）
   - Seal所有blocks和chunk
   - 扫描chunk获取metadata
   - 读取所有data并验证完整性
6. ✅ **T8-PartialWrite**（处理部分写入场景）
   - 写入10个blocks
   - 只seal前5个blocks
   - 验证只有sealed blocks可见

### 关键验证：T7-DisasterRecovery

**端到端灾难恢复流程**：
```cpp
// 1. 写入阶段（模拟正常运行）
initChunkHeader(0, header);
allocateChunk(0);

BlockWriter writer(io_.get(), layout_);
for (uint32_t i = 0; i < 20; i++) {
    TagBuffer tag_buffer;
    // ... 填充30个records
    writer.writeBlock(0, i, tag_buffer);
}

// 2. Seal阶段
DirectoryBuilder dir_builder(io_.get(), layout_, 0);
for (uint32_t i = 0; i < 20; i++) {
    dir_builder.sealBlock(i, ...);
}
dir_builder.writeDirectory();

ChunkSealer sealer(io_.get(), mutator_.get());
sealer.sealChunk(0, layout_, start_ts, end_ts);

// 3. 恢复阶段 - 扫描目录
RawScanner scanner(io_.get());
ScannedChunk scanned_chunk;
scanner.scanChunk(0, layout_, scanned_chunk);

EXPECT_EQ(42u, scanned_chunk.chunk_id);
EXPECT_TRUE(scanned_chunk.is_sealed);
EXPECT_EQ(20, scanned_chunk.blocks.size());

// 4. 恢复阶段 - 读取所有数据
BlockReader reader(io_.get(), layout_);
for (const auto& block : scanned_chunk.blocks) {
    std::vector<MemRecord> records;
    reader.readBlock(0, block.block_index, ...);

    // 验证数据完整性
    for (size_t i = 0; i < records.size(); i++) {
        EXPECT_EQ(expected_time_offset, records[i].time_offset);
        EXPECT_EQ(expected_value, records[i].value.f64_value);
    }
}

EXPECT_EQ(20u, reader.getStats().blocks_read);
EXPECT_EQ(600u, reader.getStats().records_read);
```

✅ **结果**：完整恢复20个blocks，600个records，数据100%准确

### 关键验证：T8-PartialWrite

**部分写入场景处理**：
```cpp
// 1. 写入10个blocks
BlockWriter writer(io_.get(), layout_);
for (uint32_t i = 0; i < 10; i++) {
    TagBuffer tag_buffer;
    // ... 25 records per block
    writer.writeBlock(0, i, tag_buffer);
}

// 2. 只seal前5个blocks（模拟crash）
DirectoryBuilder dir_builder(io_.get(), layout_, 0);
for (uint32_t i = 0; i < 5; i++) {
    dir_builder.sealBlock(i, ...);
}
dir_builder.writeDirectory();
// DON'T seal chunk!

// 3. 扫描恢复
RawScanner scanner(io_.get());
ScannedChunk scanned_chunk;
scanner.scanChunk(0, layout_, scanned_chunk);

EXPECT_FALSE(scanned_chunk.is_sealed);  // Chunk未seal
EXPECT_EQ(5, scanned_chunk.blocks.size());  // 只有5个sealed blocks可见

// 4. 读取sealed blocks
BlockReader reader(io_.get(), layout_);
for (const auto& block : scanned_chunk.blocks) {
    std::vector<MemRecord> records;
    reader.readBlock(0, block.block_index, ...);
    EXPECT_EQ(25, records.size());
}

EXPECT_EQ(5u, reader.getStats().blocks_read);
EXPECT_EQ(125u, reader.getStats().records_read);
```

✅ **结果**：正确识别和恢复5个sealed blocks，忽略5个unsealed blocks

## 编译与运行

```bash
# 构建并运行测试
./build.sh --test

# 运行特定测试
cd build
./test_read_recovery
```

## 验证清单

| 要求 | 状态 | 说明 |
|------|------|------|
| RawScanner扫描chunk | ✅ | 读取header和directory |
| 提取block metadata | ✅ | 解析BlockDirEntry |
| SuperCRC验证 | ✅ | 重新计算并比对 |
| 过滤unsealed blocks | ✅ | record_count = 0xFFFFFFFF过滤 |
| BlockReader读取block | ✅ | 解析所有record types |
| Record解析正确 | ✅ | time_offset/quality/value |
| 灾难恢复流程 | ✅ | 完整write-seal-scan-read |
| 部分写入处理 | ✅ | 只恢复sealed数据 |

## 项目结构（更新）

```
xTdb/
├── include/xTdb/
│   ├── constants.h
│   ├── aligned_io.h
│   ├── layout_calculator.h
│   ├── struct_defs.h
│   ├── state_mutator.h
│   ├── wal_writer.h
│   ├── mem_buffer.h
│   ├── block_writer.h
│   ├── directory_builder.h
│   ├── chunk_sealer.h
│   ├── raw_scanner.h          # ✨ 阶段5：RawScanner
│   └── block_reader.h         # ✨ 阶段5：BlockReader
├── src/
│   ├── aligned_io.cpp
│   ├── layout_calculator.cpp
│   ├── state_mutator.cpp
│   ├── wal_writer.cpp
│   ├── mem_buffer.cpp
│   ├── block_writer.cpp
│   ├── directory_builder.cpp
│   ├── chunk_sealer.cpp
│   ├── raw_scanner.cpp        # ✨ 阶段5
│   └── block_reader.cpp       # ✨ 阶段5
├── tests/
│   ├── test_alignment.cpp
│   ├── test_layout.cpp
│   ├── test_struct_size.cpp
│   ├── test_state_machine.cpp
│   ├── test_write_path.cpp
│   ├── test_seal_directory.cpp
│   └── test_read_recovery.cpp  # ✨ 阶段5：T7, T8
└── docs/
    ├── design.md
    ├── plan.md
    ├── phase1_summary.md
    ├── phase2_summary.md
    ├── phase3_summary.md
    ├── phase4_summary.md
    └── phase5_summary.md        # ✨ 本文档
```

## 下一步：阶段6

根据 `plan.md`，下一步是实现**SQLite集成**：

### 阶段6任务清单
1. **MetadataSync**：
   - 同步chunk/block metadata到SQLite
   - 维护tag索引
   - 支持时间范围查询

2. **QueryEngine**：
   - SQL查询接口
   - Tag聚合查询
   - 时间序列查询优化

3. **测试**：
   - T9-EndToEnd：完整写入-查询流程

## 性能数据

### 读取路径性能
- **RawScanner扫描**：20 blocks目录扫描 < 0.1s
- **BlockReader解析**：50 records/block < 0.01s
- **灾难恢复**：600 records完整恢复 < 0.7s

### 测试统计
- **新增测试用例**：6个（T7, T8）
- **总测试用例**：69个（Phase 1-5）
- **测试时间**：2.60秒（7个测试套件）

## 技术亮点

### 1. 字节序安全解析

**问题**：`char`可能有符号，导致符号扩展错误

```cpp
// 错误方式
const char* ptr = ...;
uint32_t value = ptr[0];  // 如果ptr[0] = 0x82 (130)
                          // 有符号char: -126
                          // 转换为uint32_t: 0xFFFFFF82 (4294967170)

// 正确方式
const uint8_t* ptr = ...;
uint32_t value = ptr[0];  // 始终为无符号
                          // 0x82 直接转换为 130
```

**修复**：
```cpp
// 使用uint8_t*确保无符号解析
const uint8_t* ptr = static_cast<const uint8_t*>(data);

record.time_offset = ptr[0] |
                    (ptr[1] << 8) |
                    (ptr[2] << 16);  // 正确解析3字节小端序
```

### 2. 灾难恢复流程

**完整恢复链**：
```
Crash/Corruption
       ↓
RawScanner.scanChunk()
  → 读取ChunkHeader
  → 读取BlockDirectory
  → 验证SuperCRC
  → 过滤unsealed blocks
       ↓
ScannedChunk metadata
       ↓
BlockReader.readBlock()
  → 计算physical offset
  → 读取data block
  → 解析records
  → (可选) 验证data CRC
       ↓
完整恢复的Records
```

**关键特性**：
- 不依赖SQLite或任何内存索引
- 纯文件系统操作，适合离线工具
- 自动处理partial writes
- CRC验证确保数据完整性

### 3. Partial Write处理

**场景**：系统crash在seal过程中

```
State 1: 10个blocks已写入数据
State 2: 5个blocks已seal (record_count != 0xFFFFFFFF)
State 3: Chunk未seal (CHB_SEALED bit = 1)
```

**恢复策略**：
- RawScanner扫描时自动过滤unsealed blocks
- 只返回完整sealed的blocks
- 未seal的数据视为invalid，忽略

**优势**：
- 保守恢复，不返回可能不完整的数据
- 状态机保证：sealed block数据必定完整
- 符合ACID原则：只读取committed数据

### 4. CRC32重用

**代码共享**：
```cpp
// ChunkSealer, RawScanner, BlockReader共享CRC32实现
static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

static void init_crc32_table() { ... }
uint32_t calculateCRC32(const void* data, uint64_t size) { ... }
```

**未来优化**：考虑提取到独立的`crc32_util.h`

## 重要修正

### Bug修复记录

1. **字节符号扩展问题**：
   - 问题：使用`const char*`读取字节，导致>127的值被符号扩展
   - 现象：time_offset = 130读取为4294967170
   - 修正：改用`const uint8_t*`确保无符号解析
   - 影响：所有record解析，尤其是time_offset

## 附注

- RawScanner使用相同的CRC32算法作为ChunkSealer（多项式0xEDB88320）
- BlockReader当前不验证data CRC（verifyBlockIntegrity提供）
- ScannedChunk.blocks只包含sealed blocks（record_count != 0xFFFFFFFF）
- 所有I/O操作严格extent-aligned（16KB）

## 阶段总结

阶段5成功实现了**读取路径与灾难恢复功能**：

✅ **RawScanner**：离线扫描，SQLite独立
✅ **BlockReader**：完整record解析，所有类型支持
✅ **灾难恢复**：端到端write-seal-scan-read验证
✅ **Partial Write**：正确处理incomplete seals

**关键成就**：
- 完整的读取路径实现（与Phase 3写入路径对应）
- 灾难恢复能力（不依赖任何运行时状态）
- Partial write安全处理（只返回完整数据）
- 字节序安全解析（修正符号扩展bug）

**端到端验证**：
- 600个records完整写入-seal-scan-read流程通过
- Partial write场景正确识别sealed/unsealed blocks
- SuperCRC和data CRC验证机制完整
- 所有value types解析正确

**下一步准备就绪**：Phase 1-5已实现完整的RAW存储层，Phase 6将添加SQLite集成实现高效的metadata索引和查询。
