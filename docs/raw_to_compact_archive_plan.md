# RAW 到 COMPACT 归档方案

## 核心思路

### 问题分析

**现状**：

- RAW 阶段：固定大小的 block（16K/64K/256K），可能已使用 Swinging Door 或 16-bit 量化压缩
- 归档需求：将 RAW 数据压缩归档，节省长期存储空间
- 查询需求：需要支持从 RAW 或 COMPACT 读取数据

**目标**：

- RAW 阶段：使用 Swinging Door 和 16-bit 量化（如果配置了）
- COMPACT_VAR 阶段：将每个 RAW block 单独压缩归档（1:1 映射），使用通用压缩（zlib/gzip）
- 不合并 block：保持 block 级别的映射关系，metadata 结构清晰

**归档策略**:

- 保持1到2天(可配置)的当前数据，以RAW形式存储
- 启动后台线程对旧数据以block为单位进行压缩归档，确保整个过程是可恢复的（中断后可恢复或者可重做）
- 完成压缩存储的block在meta（sqlite）里只需要修改对应记录的chunkid和block offset
- 回收对应的RAW block

---

## 架构设计

### 1. RAW 阶段（LAYOUT_RAW_FIXED）

**存储方式**：

```
Container (RAW_FIXED)
  └── Chunk 0
      ├── Meta Region
      └── Data Region
          ├── Block 0 (Tag 1000, 16KB, 可能已用 Swinging Door 压缩)
          ├── Block 1 (Tag 1000, 16KB, 可能已用 16-bit 量化)
          ├── Block 2 (Tag 1000, 16KB, RAW)
          └── Block 3 (Tag 1001, 16KB, Swinging Door)
```

**压缩策略**：

- **Swinging Door**：如果 tag 配置了 `preferred_encoding = ENC_SWINGING_DOOR`
- **16-bit 量化**：如果 tag 配置了 `preferred_encoding = ENC_QUANTIZED_16` 且配置了 `low_extreme/high_extreme`
- **RAW**：如果 tag 没有配置压缩

**Metadata 记录**（RAW）：

```sql
-- blocks 表：每个 RAW block 一条记录
tag_id=1000, chunk_id=0, block_index=0, encoding_type=ENC_SWINGING_DOOR
tag_id=1000, chunk_id=0, block_index=1, encoding_type=ENC_QUANTIZED_16
tag_id=1000, chunk_id=0, block_index=2, encoding_type=ENC_RAW
```

---

### 2. COMPACT_VAR 阶段（LAYOUT_COMPACT_VAR）

**存储方式**：

```
Container (COMPACT_VAR)
  └── Chunk 0
      ├── Meta Region (变长 block 索引)
      └── Data Region
          ├── Compact Block 0 (压缩后的 RAW Block 0，变长)
          ├── Compact Block 1 (压缩后的 RAW Block 1，变长)
          ├── Compact Block 2 (压缩后的 RAW Block 2，变长)
          └── Compact Block 3 (压缩后的 RAW Block 3，变长)
```

**转换过程**：

1. **读取**：读取一个 RAW block 的数据（可能已经过 Swinging Door 或 16-bit 量化压缩）
2. **压缩**：使用通用压缩算法（zlib/gzip）对 RAW block 的字节流进行无损压缩
3. **存储**：写入 COMPACT_VAR container 的变长 block（1:1 映射）

**Metadata 记录**（COMPACT）：

```sql
-- blocks 表：每个 COMPACT block 对应一个 RAW block
-- 原始 RAW block 记录标记为已归档或删除
tag_id=1000, chunk_id=0, block_index=0, container_id=COMPACT_CONTAINER, is_archived=1
tag_id=1000, chunk_id=0, block_index=1, container_id=COMPACT_CONTAINER, is_archived=1
tag_id=1000, chunk_id=0, block_index=2, container_id=COMPACT_CONTAINER, is_archived=1
```

---

## 压缩策略说明

### RAW 阶段压缩

**Swinging Door 压缩**：

- **目的**：时间维度压缩，减少数据点数量
- **适用场景**：连续采集量（Float, Int），有工程容差要求
- **压缩比**：10:1 至 100:1（取决于数据特性）
- **精度损失**：在工程容差范围内（有损压缩）

**16-bit 量化压缩**：

- **目的**：数值维度压缩，减少数值精度
- **适用场景**：物理量程已知的数据（如温度 0-100℃）
- **压缩比**：4:1（F64 → I16）
- **精度损失**：可控（0.0015% 以内）

**组合使用**：

- 可以同时使用 Swinging Door 和 16-bit 量化
- 先进行 Swinging Door 压缩（减少点数），再进行 16-bit 量化（减少精度）

---

### COMPACT 阶段压缩

**通用压缩（zlib/gzip）**：

- **目的**：对已压缩的数据进行进一步压缩，节省存储空间
- **适用场景**：归档存储，长期保存
- **压缩算法**：zlib/gzip 等通用无损压缩
- **压缩比**：20-50%（取决于数据特性）

**压缩对象**：

- RAW block 的字节流（可能已经过 Swinging Door 或 16-bit 量化压缩）
- 对压缩后的字节流进行通用压缩，进一步减少存储空间

**为什么不合并 block**：

- 保持 block 级别的映射关系（1:1）
- metadata 结构清晰，易于管理
- 查询时可以精确定位到具体的 block
- 避免跨 block 合并带来的复杂性

---

## 关于"重复压缩"的解答

### 问题：RAW 阶段已经用了 Swinging Door 或 16-bit 量化，再压缩有意义吗？

**答案：有意义**

#### 1. Swinging Door 压缩后的数据特点

- **压缩后**：数据点已经减少（线性段的关键端点）
- **但仍有冗余**：
  - 时间戳的增量编码模式
  - 数值的重复模式
  - 字节对齐的 padding
- **通用压缩**：可以进一步压缩这些冗余模式

#### 2. 16-bit 量化压缩后的数据特点

- **压缩后**：数值精度已降低（F64 → I16）
- **但仍有冗余**：
  - I16 数值的重复模式
  - 时间戳的增量编码
- **通用压缩**：可以进一步压缩这些冗余模式

#### 3. 通用压缩的效果

- **Swinging Door 压缩后的数据**：通用压缩通常可以获得 20-40% 的额外压缩
- **16-bit 量化压缩后的数据**：通用压缩通常可以获得 30-50% 的额外压缩
- **RAW 数据**：通用压缩通常可以获得 40-60% 的压缩

**结论**：即使 RAW 阶段已经压缩，通用压缩仍然有意义，可以进一步节省 20-50% 的存储空间。

---

## 实施计划

### 阶段 1：COMPACT_VAR Container 实现

**目标**：实现 COMPACT_VAR layout 的 Container

**任务**：

- [ ] 扩展 `ContainerHeaderV12`，支持 COMPACT_VAR layout
- [ ] 设计 COMPACT_VAR chunk 结构（变长 block 索引）
- [ ] 实现 COMPACT_VAR Container 的读写逻辑

**关键设计**：

```cpp
// COMPACT_VAR Chunk 结构
struct CompactChunkHeader {
    uint32_t chunk_id;
    uint32_t block_count;        // 变长 block 数量
    uint64_t data_offset;        // 数据区起始偏移
    uint64_t index_offset;       // 索引区起始偏移
    // ... 其他字段
};

// 变长 Block 索引项（1:1 对应 RAW block）
struct CompactBlockIndex {
    uint32_t tag_id;
    uint32_t original_chunk_id;  // 原始 RAW chunk_id
    uint32_t original_block_index; // 原始 RAW block_index
    uint64_t data_offset;        // 变长 block 数据偏移
    uint32_t data_size;          // 变长 block 大小（压缩后）
    uint32_t original_size;      // 原始 RAW block 大小
    int64_t start_ts_us;
    int64_t end_ts_us;
    uint8_t encoding_type;       // 原始编码类型（ENC_SWINGING_DOOR/ENC_QUANTIZED_16/ENC_RAW）
    uint8_t compression_type;    // COMPACT 压缩类型（ZLIB/GZIP）
};
```

---

### 阶段 2：RAW 到 COMPACT 转换引擎

**目标**：实现将 RAW blocks 转换为 COMPACT blocks 的引擎

**任务**：

- [ ] 实现 `CompactArchiver` 类
- [ ] 读取单个 RAW block 数据
- [ ] 使用 zlib/gzip 压缩
- [ ] 写入 COMPACT_VAR container（1:1 映射）

**关键接口**：

```cpp
class CompactArchiver {
public:
    // 归档单个 RAW block 到 COMPACT container
    ArchiveResult archiveBlock(uint32_t container_id,
                                uint32_t chunk_id,
                                uint32_t block_index,
                                uint32_t compact_container_id);
    
    // 批量归档（按时间窗口）
    ArchiveResult archiveByTimeWindow(int64_t window_start_us,
                                       int64_t window_end_us,
                                       uint32_t compact_container_id);
    
    // 归档指定 tag 的所有 blocks
    ArchiveResult archiveTag(uint32_t tag_id,
                              uint32_t compact_container_id);
};
```

**压缩流程**：

```
1. 读取 RAW block 数据
   └── 从 RAW container 读取 block 字节流（可能已压缩）
2. 通用压缩
   └── 使用 zlib/gzip 压缩字节流
3. 写入 COMPACT container
   └── 写入变长 block，记录索引项
```

---

### 阶段 3：Metadata 更新

**目标**：实现 metadata 的更新逻辑

**任务**：

- [ ] 为每个 COMPACT block 创建 metadata 记录
- [ ] 标记原始 RAW block 记录为已归档（或删除）
- [ ] 实现查询时的透明路由（RAW 或 COMPACT）

**Metadata 更新流程**：

```sql
-- 1. 插入 COMPACT block 记录
INSERT INTO blocks (container_id, chunk_id, block_index, tag_id, 
                    original_chunk_id, original_block_index,
                    encoding_type, compression_type, is_archived, ...) 
VALUES (...);

-- 2. 标记原始 RAW block 记录为已归档（软删除）
UPDATE blocks 
SET is_archived = 1, archived_to_container_id = ?, archived_to_chunk_id = ?, archived_to_block_index = ?
WHERE container_id = ? AND chunk_id = ? AND block_index = ?;

-- 或者删除原始 RAW block 记录（硬删除）
DELETE FROM blocks 
WHERE container_id = ? AND chunk_id = ? AND block_index = ?;
```

---

### 阶段 4：查询路由优化

**目标**：实现查询时自动选择 RAW 或 COMPACT

**任务**：

- [ ] 实现查询路由逻辑：优先查询 RAW，如果已归档则查询 COMPACT
- [ ] 实现 COMPACT block 的解压缩读取
- [ ] 实现透明查询（用户无需关心数据在 RAW 还是 COMPACT）

**查询逻辑**：

```
查询 tag=1000, time_range=[T1, T2]
  ├── 1. 查询 RAW blocks（优先）
  │   ├── 如果存在且未归档，直接返回
  │   └── 如果已归档（is_archived=1），跳转到 COMPACT
  ├── 2. 查询 COMPACT blocks
  │   ├── 根据 archived_to_* 字段定位 COMPACT block
  │   ├── 读取 COMPACT block 数据
  │   ├── 解压缩（zlib/gzip）
  │   └── 根据 encoding_type 解码（Swinging Door/Quantized16）
  └── 3. 返回数据
```

---

### 阶段 5：归档策略配置

**目标**：实现灵活的归档策略

**任务**：

- [ ] 配置归档触发条件（时间窗口、数据量阈值）
- [ ] 配置压缩算法选择（zlib/gzip）
- [ ] 实现后台归档服务

**配置示例**：

```cpp
struct ArchivePolicy {
    int64_t archive_window_days;      // 归档时间窗口（天）
    uint64_t min_age_days;            // 最小归档年龄（天）
    CompressionType compact_compression;  // COMPACT 压缩算法（ZLIB/GZIP）
    bool delete_raw_after_archive;    // 归档后是否删除 RAW（硬删除）
};
```

---

## 关键文件修改

### 1. 数据结构扩展

**`include/xTdb/struct_defs.h`**：

- 扩展 `ContainerHeaderV12` 支持 COMPACT_VAR
- 新增 `CompactChunkHeader` 结构
- 新增 `CompactBlockIndex` 结构

### 2. Container 实现

**`include/xTdb/container.h`**：

- 扩展 `IContainer` 接口，支持 COMPACT_VAR layout

**新建 `src/compact_container.cpp`**：

- 实现 COMPACT_VAR layout 的 Container

### 3. 归档引擎

**新建 `include/xTdb/compact_archiver.h`**：

- `CompactArchiver` 类定义

**新建 `src/compact_archiver.cpp`**：

- 实现 RAW 到 COMPACT 的转换逻辑（1:1 映射）
- 实现 zlib/gzip 压缩接口

### 4. Metadata 同步

**`src/metadata_sync.cpp`**：

- 扩展 `MetadataSync`，支持 COMPACT block 的 metadata 管理
- 实现 metadata 更新逻辑（标记/删除 RAW block 记录）

### 5. 查询路由

**`src/storage_engine.cpp`** 或新建查询路由模块：

- 实现查询路由逻辑（RAW → COMPACT）
- 实现 COMPACT block 的解压缩读取

---

## 总结

### 核心优势

1. **进一步压缩空间**：

   - RAW 阶段：使用 Swinging Door 或 16-bit 量化（如果配置了）
   - COMPACT 阶段：使用通用压缩（zlib/gzip），进一步压缩 20-50%
   - 总体压缩比：根据配置不同，可达到 10:1 至 200:1

2. **保持 block 映射关系**：

   - 1:1 映射：一个 RAW block 对应一个 COMPACT block
   - metadata 结构清晰，易于管理
   - 查询时可以精确定位

3. **灵活的压缩策略**：

   - RAW 阶段：根据 tag 配置选择压缩算法（Swinging Door/Quantized16/RAW）
   - COMPACT 阶段：统一使用通用压缩（zlib/gzip）

### 关于"重复压缩"

- **Swinging Door → zlib/gzip**：有意义，可以进一步压缩 20-40%
- **16-bit 量化 → zlib/gzip**：有意义，可以进一步压缩 30-50%
- **RAW → zlib/gzip**：有意义，可以压缩 40-60%
- **推荐策略**：RAW 阶段使用 Swinging Door/Quantized16（如果配置了），COMPACT 阶段使用 zlib/gzip