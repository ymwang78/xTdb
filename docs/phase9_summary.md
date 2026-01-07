# xTdb Phase 9 Summary: Read Path Coordinator

**完成时间**: 2026-01-07
**状态**: ✅ 完成

## 阶段目标

实现读取路径的全链路协调，包括：
- 查询内存缓冲区（未落盘数据）
- 扫描磁盘块（已封存数据）
- 混合读取并按时间戳排序

## 实现模块

### 9.1 核心功能

#### `StorageEngine::queryPoints()`
- **功能**: 读取指定 tag_id 和时间范围的数据点
- **步骤**:
  1. 从内存缓冲区读取未落盘数据
  2. 使用 RawScanner 扫描磁盘块
  3. 使用 BlockReader 读取匹配的数据块
  4. 归并排序结果按时间戳排序
- **文件**: `src/storage_engine.cpp:551-637`

#### 关键修复

在Phase 9实现过程中，修复了多个关键的alignment问题：

1. **Layout Configuration Bug** (constructor)
   - **问题**: 构造函数无条件覆盖用户提供的layout配置
   - **修复**: 添加条件逻辑保留用户配置
   - **位置**: `src/storage_engine.cpp:13-27`

2. **BlockWriter Offset Bug**
   - **问题**: 参数混用chunk_id（逻辑ID）和chunk_offset（物理偏移）
   - **修复**: 统一使用chunk_offset进行直接偏移计算
   - **位置**: `include/xTdb/block_writer.h:50`, `src/block_writer.cpp:91-105`

3. **Directory Placement Issue**
   - **问题**: DirectoryBuilder错误计算目录偏移，导致覆盖数据块
   - **修复**: 目录从chunk_offset + kChunkHeaderSize开始，使用read-modify-write模式
   - **位置**: `src/directory_builder.cpp:95-124`

4. **Alignment Violations**
   - **问题**: DirectoryBuilder和RawScanner尝试从未对齐偏移读取
   - **修复**: 统一使用read-modify-write模式：读取整个meta region，提取目录部分
   - **位置**: `src/directory_builder.cpp:28-58`, `src/raw_scanner.cpp:85-109`

### 9.2 读取统计

新增读取统计指标：
```cpp
struct ReadStats {
    uint64_t queries_executed;    // 执行的查询数
    uint64_t points_read_memory;  // 从内存读取的点数
    uint64_t points_read_disk;    // 从磁盘读取的点数
    uint64_t blocks_read;         // 读取的块数
};
```

## 测试覆盖

### T12-HybridRead (ReadCoordinatorTest)

**场景**: 混合读取内存和磁盘数据
- 写入 1001 个点（触发自动flush，1000点落盘）
- 再写入 49 个点（保留在内存）
- 查询跨越磁盘和内存的时间范围（900-1040）

**验证点**:
- ✅ 从磁盘读取 100 个点（900-999）
- ✅ 从内存读取 41 个点（1000-1040）
- ✅ 总计 141 个点
- ✅ 结果按时间戳排序
- ✅ 数据正确性验证

### 其他读取测试

- **QueryFromMemory**: 仅从内存缓冲区查询
- **QueryFromDisk**: 仅从磁盘块查询
- **TimeRangeFilter**: 时间范围过滤
- **QueryNonExistentTag**: 查询不存在的tag
- **MultipleTagsQuery**: 多tag查询

## 测试结果

```
ReadCoordinatorTest: 6/6 PASS (100%)
  ✅ QueryFromMemory
  ✅ QueryFromDisk
  ✅ T12_HybridRead
  ✅ TimeRangeFilter
  ✅ QueryNonExistentTag
  ✅ MultipleTagsQuery
```

## 技术要点

### 16KB对齐约束

所有磁盘读取必须满足：
- **Buffer地址**: 16KB对齐 (通过AlignedBuffer保证)
- **读取大小**: 16KB的整数倍
- **偏移量**: 16KB的整数倍

### Read-Modify-Write模式

对于更新meta region中的目录：
```cpp
// 1. 读取整个meta region (对齐)
AlignedBuffer buffer(meta_region_size);
io_->read(buffer.data(), meta_region_size, chunk_offset);

// 2. 更新内存中的目录部分
std::memcpy(buffer.data() + kChunkHeaderSize,
            entries.data(),
            dir_size_bytes);

// 3. 写回整个meta region (对齐)
io_->write(buffer.data(), meta_region_size, chunk_offset);
```

### 时间戳计算

- **Block时间范围**: `[start_ts_us, end_ts_us]`
- **Record时间戳**: `start_ts_us + time_offset * 1000` (ms → μs)
- **过滤逻辑**: `timestamp >= query_start && timestamp <= query_end`

## 性能指标

- **测试执行时间**: ~8ms (6个测试)
- **混合读取延迟**: <5ms (141个点)
- **内存读取**: 零额外IO
- **磁盘读取**: 单次16KB对齐读取

## 下一步工作

**Phase 10**: 后台服务（Maintenance Services）
- Directory Syncer (目录同步器)
- Retention Service (过期清理)
- 定时任务框架

## 关键文件清单

### 实现文件
- `src/storage_engine.cpp`: queryPoints()实现, 统计追踪
- `src/block_reader.cpp`: 数据块读取
- `src/raw_scanner.cpp`: 磁盘扫描, directory读取修复
- `src/directory_builder.cpp`: load()方法, alignment修复

### 头文件
- `include/xTdb/storage_engine.h`: ReadStats定义, queryPoints声明
- `include/xTdb/block_reader.h`: BlockReader接口
- `include/xTdb/raw_scanner.h`: RawScanner接口
- `include/xTdb/block_writer.h`: 签名修复(chunk_offset)

### 测试文件
- `tests/test_read_coordinator.cpp`: 6个读取路径测试
- `tests/test_hybrid_debug.cpp`: 混合读取调试工具

## 经验教训

1. **Alignment First**: 所有磁盘操作必须从设计阶段就考虑16KB对齐
2. **Physical vs Logical**: 区分物理偏移(chunk_offset)和逻辑ID(chunk_id)
3. **Read-Modify-Write**: 是满足对齐约束的标准模式
4. **Test-Driven Debugging**: 创建独立调试测试帮助快速定位问题
5. **Complete Discovery**: 修复一个alignment问题时，同时检查所有类似代码路径

---

**Phase 9 完成标志**:
- ✅ 所有读取路径测试通过
- ✅ 混合读取正确工作
- ✅ Alignment问题全部修复
- ✅ 读取统计正确追踪
